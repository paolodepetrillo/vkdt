#include "graph.h"
#include "graph-io.h"
#include "draw.h"
#include "module.h"
#include "cycles.h"
#include "modules/api.h"
#include "modules/localsize.h"
#include "core/log.h"
#include "qvk/qvk.h"
#include "graph-print.h"
#ifdef DEBUG_MARKERS
#include "db/stringpool.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMG_LAYOUT(img, oli, nli) do {\
  if(!img->image) break;\
  VkImageLayout nl = VK_IMAGE_LAYOUT_ ## nli;\
  if(nl != img->layout)\
    BARRIER_IMG_LAYOUT(img->image, img->layout, nl);\
  img->layout = nl;\
} while(0)

void
dt_graph_init(dt_graph_t *g)
{
  memset(g, 0, sizeof(*g));
#ifdef DEBUG_MARKERS
  dt_stringpool_init(&g->debug_markers, 2000, 20);
#endif

  g->frame_cnt = 1;

  // allocate module and node buffers:
  g->max_modules = 100;
  g->module = calloc(sizeof(dt_module_t), g->max_modules);
  g->max_nodes = 4000;
  g->node = calloc(sizeof(dt_node_t), g->max_nodes);
  dt_vkalloc_init(&g->heap, 16000, ((uint64_t)1)<<40); // bytesize doesn't matter
  dt_vkalloc_init(&g->heap_ssbo, 8000, ((uint64_t)1)<<40);
  dt_vkalloc_init(&g->heap_staging, 100, ((uint64_t)1)<<40);
  g->params_max = 16u<<20;
  g->params_end = 0;
  g->params_pool = calloc(sizeof(uint8_t), g->params_max);

  g->conn_image_max = 30*2*2000; // connector images for a lot of connectors
  g->conn_image_end = 0;
  g->conn_image_pool = calloc(sizeof(dt_connector_image_t), g->conn_image_max);

  VkCommandPoolCreateInfo cmd_pool_create_info = {
    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = g->queue_idx,
    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
  };
  QVK(vkCreateCommandPool(qvk.device, &cmd_pool_create_info, NULL, &g->command_pool));

  VkCommandBufferAllocateInfo cmd_buf_alloc_info = {
    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool        = g->command_pool,
    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 2,
  };
  QVK(vkAllocateCommandBuffers(qvk.device, &cmd_buf_alloc_info, g->command_buffer));
  VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };
  QVK(vkCreateFence(qvk.device, &fence_info, NULL, g->command_fence+0));
  QVK(vkCreateFence(qvk.device, &fence_info, NULL, g->command_fence+1));
  g->float_atomics_supported = qvk.float_atomics_supported;

  for(int i=0;i<2;i++)
  {
    g->query[i].max = 2000;
    g->query[i].cnt = 0;
    VkQueryPoolCreateInfo query_pool_info = {
      .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType  = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = g->query[i].max,
    };
    QVK(vkCreateQueryPool(qvk.device, &query_pool_info, NULL, &g->query[i].pool));
    g->query[i].pool_results = malloc(sizeof(uint64_t)*g->query[i].max);
    g->query[i].name   = malloc(sizeof(dt_token_t)*g->query[i].max);
    g->query[i].kernel = malloc(sizeof(dt_token_t)*g->query[i].max);
  }

  // grab default queue:
  g->queue = qvk.queue_compute;
  g->queue_idx = qvk.queue_idx_compute;
  g->queue_mutex = &qvk.queue_mutex;

  g->lod_scale = 1;
  g->active_module = -1;
}

void
dt_graph_cleanup(dt_graph_t *g)
{
  if(!g->module) return; // already cleaned up
#ifdef DEBUG_MARKERS
  dt_stringpool_cleanup(&g->debug_markers);
#endif
  QVKL(&qvk.queue_mutex, vkDeviceWaitIdle(qvk.device));
  for(int i=0;i<g->num_modules;i++)
    if(g->module[i].name && g->module[i].so->cleanup)
      g->module[i].so->cleanup(g->module+i);
  for(int i=0;i<g->num_modules;i++)
  {
    free(g->module[i].keyframe);
    g->module[i].keyframe_size = 0;
    g->module[i].keyframe_cnt = 0;
    g->module[i].keyframe = 0;
  }
  dt_vkalloc_cleanup(&g->heap);
  dt_vkalloc_cleanup(&g->heap_ssbo);
  dt_vkalloc_cleanup(&g->heap_staging);
  for(int i=0;i<g->num_nodes;i++)
  {
    for(int j=0;j<g->node[i].num_connectors;j++)
    {
      dt_connector_t *c = g->node[i].connector+j;
      if(c->flags & s_conn_dynamic_array)
      { // free potential double images for multi-frame dsets
        for(int k=0;k<MAX(1,c->array_length);k++)
        { // avoid error message in case image does not exist:
          dt_connector_image_t *img0 = (g->node[i].conn_image[j] != -1) ? dt_graph_connector_image(g, i, j, k, 0) : 0;
          dt_connector_image_t *img1 = (g->node[i].conn_image[j] != -1) ? dt_graph_connector_image(g, i, j, k, 1) : 0;
          if(!img0 || !img1) continue;
          if(img0->image == img1->image)
          { // it's enough to clean up one replicant, the rest will shut down cleanly later:
            *img0 = (dt_connector_image_t){0};
          }
        }
      }
    }
  }
  for(int i=0;i<g->conn_image_end;i++)
  {
    if(g->conn_image_pool[i].buffer)     vkDestroyBuffer(qvk.device,    g->conn_image_pool[i].buffer, VK_NULL_HANDLE);
    if(g->conn_image_pool[i].image)      vkDestroyImage(qvk.device,     g->conn_image_pool[i].image,  VK_NULL_HANDLE);
    if(g->conn_image_pool[i].image_view) vkDestroyImageView(qvk.device, g->conn_image_pool[i].image_view, VK_NULL_HANDLE);
    g->conn_image_pool[i].buffer = 0;
    g->conn_image_pool[i].image = 0;
    g->conn_image_pool[i].image_view = 0;
  }
  for(int i=0;i<g->num_nodes;i++)
  {
    for(int j=0;j<g->node[i].num_connectors;j++)
    {
      dt_connector_t *c = g->node[i].connector+j;
      if(c->staging) vkDestroyBuffer(qvk.device, c->staging, VK_NULL_HANDLE);
      c->staging = 0;
      if(c->array_alloc)
      { // free any potential residuals of dynamic allocation
        dt_vkalloc_cleanup(c->array_alloc);
        free(c->array_alloc);
        c->array_alloc = 0;
        c->array_alloc_size = 0;
        c->array_mem = 0;
      }
    }
    vkDestroyPipelineLayout     (qvk.device, g->node[i].pipeline_layout,  0);
    vkDestroyPipeline           (qvk.device, g->node[i].pipeline,         0);
    vkDestroyDescriptorSetLayout(qvk.device, g->node[i].dset_layout,      0);
    vkDestroyFramebuffer        (qvk.device, g->node[i].draw_framebuffer, 0);
    vkDestroyRenderPass         (qvk.device, g->node[i].draw_render_pass, 0);
    g->node[i].pipeline_layout = 0;
    g->node[i].pipeline = 0;
    g->node[i].dset_layout = 0;
    g->node[i].draw_framebuffer = 0;
    g->node[i].draw_render_pass = 0;
    dt_raytrace_node_cleanup(g->node + i);
  }
  dt_raytrace_graph_cleanup(g);
  vkDestroyDescriptorPool(qvk.device, g->dset_pool, 0);
  vkDestroyDescriptorSetLayout(qvk.device, g->uniform_dset_layout, 0);
  vkDestroyBuffer(qvk.device, g->uniform_buffer, 0);
  g->dset_pool = 0;
  g->uniform_dset_layout = 0;
  g->uniform_buffer = 0;
  vkFreeMemory(qvk.device, g->vkmem, 0);
  vkFreeMemory(qvk.device, g->vkmem_ssbo, 0);
  vkFreeMemory(qvk.device, g->vkmem_staging, 0);
  vkFreeMemory(qvk.device, g->vkmem_uniform, 0);
  g->vkmem = g->vkmem_ssbo = g->vkmem_staging = g->vkmem_uniform = 0;
  g->vkmem_size = g->vkmem_ssbo_size = g->vkmem_staging_size = g->vkmem_uniform_size = 0;
  vkDestroyFence(qvk.device, g->command_fence[0], 0);
  vkDestroyFence(qvk.device, g->command_fence[1], 0);
  g->command_fence[0] = 0;
  g->command_fence[1] = 0;
  if(g->command_pool != VK_NULL_HANDLE)
    vkFreeCommandBuffers(qvk.device, g->command_pool, 2, g->command_buffer);
  g->command_buffer[0] = g->command_buffer[1] = VK_NULL_HANDLE;
  vkDestroyCommandPool(qvk.device, g->command_pool, 0);
  g->command_pool = 0;
  free(g->module);             g->module = 0;
  free(g->node);               g->node = 0;
  free(g->params_pool);        g->params_pool = 0;
  free(g->conn_image_pool);    g->conn_image_pool = 0;
  for(int i=0;i<2;i++)
  {
    vkDestroyQueryPool(qvk.device, g->query[i].pool, 0);
    g->query[i].pool = 0;
    free(g->query[i].pool_results); g->query[i].pool_results = 0;
    free(g->query[i].name);         g->query[i].name = 0;
    free(g->query[i].kernel);       g->query[i].kernel = 0;
  }
}

static inline void *
read_file(const char *filename, size_t *len)
{
  FILE *f = fopen(filename, "rb");
  if(!f)
  {
    dt_log(s_log_qvk|s_log_err, "failed to read shader '%s': %s!",
        filename, strerror(errno));
    return 0;
  }
  fseek(f, 0, SEEK_END);
  const size_t filesize = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *file = malloc(filesize+1);

  size_t rd = fread(file, sizeof(char), filesize, f);
  file[filesize] = 0;
  if(rd != filesize)
  {
    free(file);
    file = 0;
    fclose(f);
    return 0;
  }
  if(len) *len = filesize;
  fclose(f);
  return file;
}

VkResult
dt_graph_create_shader_module(
    dt_graph_t     *graph,
    dt_token_t      node,
    dt_token_t      kernel,
    const char     *type,
    VkShaderModule *shader_module)
{
  // create the compute shader stage
  char filename[PATH_MAX+100] = {0};
  snprintf(filename, sizeof(filename), "%s/modules/%"PRItkn"/%"PRItkn".%s.spv",
      dt_pipe.basedir, dt_token_str(node), dt_token_str(kernel), type);

  size_t len;
  void *data = read_file(filename, &len);
  if(!data) return VK_ERROR_INVALID_EXTERNAL_HANDLE;

  VkShaderModuleCreateInfo sm_info = {
    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = len,
    .pCode    = data
  };
  QVKR(vkCreateShaderModule(qvk.device, &sm_info, 0, shader_module));
  free(data);
#ifdef DEBUG_MARKERS
#ifdef QVK_ENABLE_VALIDATION
  char name[100];
  const char *dedup;
  snprintf(name, sizeof(name), "%"PRItkn"_%"PRItkn, dt_token_str(node), dt_token_str(kernel));
  dt_stringpool_get(&graph->debug_markers, name, strlen(name), 0, &dedup);
  VkDebugMarkerObjectNameInfoEXT name_info = {
    .sType = VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT,
    .object = (uint64_t) shader_module,
    .objectType = VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT,
    .pObjectName = dedup,
  };
  qvkDebugMarkerSetObjectNameEXT(qvk.device, &name_info);
#endif
#endif
  return VK_SUCCESS;
}

static inline VkFormat
dt_connector_vkformat(const dt_connector_t *c)
{
  const int len = dt_connector_channels(c);
  if(c->format == dt_token("ui32") || c->format == dt_token("u32") || (c->format == dt_token("atom") && !qvk.float_atomics_supported))
  {
    switch(len)
    { // int32 does not have UNORM equivalents (use SFLOAT instead i guess)
      case 1: return VK_FORMAT_R32_UINT;
      case 2: return VK_FORMAT_R32G32_UINT;
      case 3: // return VK_FORMAT_R32G32B32_UINT;
      case 4: return VK_FORMAT_R32G32B32A32_UINT;
    }
  }
  if(c->format == dt_token("f32") || (c->format == dt_token("atom") && qvk.float_atomics_supported))
  {
    switch(len)
    {
      case 1: return VK_FORMAT_R32_SFLOAT;          // r32f
      case 2: return VK_FORMAT_R32G32_SFLOAT;       // rg32f
      case 3: // return VK_FORMAT_R32G32B32_SFLOAT; // glsl does not support this
      case 4: return VK_FORMAT_R32G32B32A32_SFLOAT; // rgba32f
    }
  }
  if(c->format == dt_token("f16"))
  {
    switch(len)
    {
      case 1: return VK_FORMAT_R16_SFLOAT;          // r16f
      case 2: return VK_FORMAT_R16G16_SFLOAT;       // rg16f
      case 3: // return VK_FORMAT_R16G16B16_SFLOAT; // glsl does not support this
      case 4: return VK_FORMAT_R16G16B16A16_SFLOAT; // rgba16f
    }
  }
  if(c->format == dt_token("ui16") || c->format == dt_token("u16"))
  {
    switch(len)
    {
      case 1: return VK_FORMAT_R16_UNORM;
      case 2: return VK_FORMAT_R16G16_UNORM;
      case 3: // return VK_FORMAT_R16G16B16_UNORM;
      case 4: return VK_FORMAT_R16G16B16A16_UNORM;
    }
  }
  if(c->format == dt_token("ui8") || c->format == dt_token("u8"))
  {
    switch(len)
    {
      case 1: return VK_FORMAT_R8_UNORM;
      case 2: return VK_FORMAT_R8G8_UNORM;
      case 3: // return VK_FORMAT_R8G8B8_UNORM;
      case 4: return VK_FORMAT_R8G8B8A8_UNORM;
    }
  }
  if(c->format == dt_token("yuv")) return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  if(c->format == dt_token("bc1")) return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
  return VK_FORMAT_UNDEFINED;
}


static inline VkResult
allocate_image_array_element(
    dt_graph_t        *graph,
    dt_node_t         *node,
    dt_connector_t    *c,                   // the connector with an output array of images
    dt_vkalloc_t      *heap,                // allocator to ask for memory. this is graph->heap for non dynamic arrays
    uint64_t           heap_offset,         // if dynamic/inner allocator is used, this is the offset to the outer memory
    int                f,                   // frame index for array connectors
    int                k)                   // array index or 0 if no array
{
  VkFormat format = dt_connector_vkformat(c);
  VkImageCreateInfo images_create_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .flags = c->format == dt_token("yuv") ? VK_IMAGE_CREATE_DISJOINT_BIT : 0,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = format,
    .extent = { // safeguard against disconnected 0-size extents
      .width  = MAX(c->roi.wd, 1),
      .height = MAX(c->roi.ht, 1),
      .depth  = 1
    },
    .mipLevels             = 1,
    .arrayLayers           = 1,
    .samples               = VK_SAMPLE_COUNT_1_BIT,
    .tiling                = c->format == dt_token("yuv") ?
      VK_IMAGE_TILING_LINEAR:
      VK_IMAGE_TILING_OPTIMAL,
    .usage                 = 
      c->format == dt_token("bc1") || c->format == dt_token("yuv") ?
      VK_IMAGE_ASPECT_COLOR_BIT
      | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_SAMPLED_BIT :
      VK_IMAGE_USAGE_STORAGE_BIT
      | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
      | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_SAMPLED_BIT
      | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
    // we will potentially need access to these images from both graphics and compute pipeline:
    .sharingMode           = VK_SHARING_MODE_EXCLUSIVE, // VK_SHARING_MODE_CONCURRENT, 
    // .queueFamilyIndexCount = 0,//2,
    // .pQueueFamilyIndices   = 0,//queues,
    .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  uint32_t wd = MAX(1, c->roi.wd), ht = MAX(1, c->roi.ht);
  // fprintf(stderr, "DEBUG allocating imag %"PRItkn":%"PRItkn":%"PRItkn":%"PRItkn" %d x %d\n", dt_token_str(node->name), dt_token_str(node->module->inst), dt_token_str(node->kernel), dt_token_str(c->name), wd, ht);
  if(c->array_dim)
  { // array with varying size images?
    wd = MAX(1, c->array_dim[2*k+0]);
    ht = MAX(1, c->array_dim[2*k+1]);
  }
  images_create_info.extent.width  = wd;
  images_create_info.extent.height = ht;
  dt_connector_image_t *img = dt_graph_connector_image(graph,
      node - graph->node, c - node->connector, k, f);
  if(img->image)  vkDestroyImage (qvk.device, img->image,  VK_NULL_HANDLE);
  if(img->buffer) vkDestroyBuffer(qvk.device, img->buffer, VK_NULL_HANDLE);
  img->image  = 0;
  img->buffer = 0;
  img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  QVKR(vkCreateImage(qvk.device, &images_create_info, NULL, &img->image));
#ifdef DEBUG_MARKERS
#ifdef QVK_ENABLE_VALIDATION
  char name[100];
  const char *dedup;
  snprintf(name, sizeof(name), "%"PRItkn"_%"PRItkn"_%"PRItkn"@%d,%d", dt_token_str(node->module->name), dt_token_str(node->kernel), dt_token_str(c->name), f, k);
  dt_stringpool_get(&graph->debug_markers, name, strlen(name), 0, &dedup);
  VkDebugMarkerObjectNameInfoEXT name_info = {
    .sType = VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT,
    .object = (uint64_t) img->image,
    .objectType = VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT,
    .pObjectName = dedup,
  };
  qvkDebugMarkerSetObjectNameEXT(qvk.device, &name_info);
#endif
#endif

  VkMemoryRequirements mem_req;
  if(c->format == dt_token("yuv"))
  { // get memory requirements for each plane and combine
    VkImagePlaneMemoryRequirementsInfo plane_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
      .planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT,
    };
    VkImageMemoryRequirementsInfo2 image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .pNext = &plane_info,
      .image = img->image,
    };
    VkMemoryRequirements2 memory_requirements2 = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vkGetImageMemoryRequirements2(qvk.device, &image_info, &memory_requirements2);
    mem_req = memory_requirements2.memoryRequirements;
    // plane 1
    plane_info.planeAspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
    vkGetImageMemoryRequirements2(qvk.device, &image_info, &memory_requirements2);
    // TODO: plus alignment gaps?
    img->plane1_offset = mem_req.size;
    mem_req.size += memory_requirements2.memoryRequirements.size;
    mem_req.memoryTypeBits |= memory_requirements2.memoryRequirements.memoryTypeBits;
  }
  else
  {
    vkGetImageMemoryRequirements(qvk.device, img->image, &mem_req);
  }
  if(graph->memory_type_bits != ~0 && mem_req.memoryTypeBits != graph->memory_type_bits)
  {
    dt_log(s_log_qvk|s_log_err, "node %"PRItkn" %"PRItkn" %"PRItkn":",
        dt_token_str(node->module->name), dt_token_str(node->kernel), dt_token_str(c->name));
    dt_log(s_log_qvk|s_log_err, "image memory type bits don't match! %d %d", mem_req.memoryTypeBits, graph->memory_type_bits);
  }
  graph->memory_type_bits = mem_req.memoryTypeBits;

  assert(!(mem_req.alignment & (mem_req.alignment - 1)));

  if(heap_offset == 0 && (c->frames == 2 || c->type == dt_token("source") || (c->flags & s_conn_protected))) // allocate protected memory, only in outer heap
    img->mem = dt_vkalloc_feedback(heap, mem_req.size, mem_req.alignment);
  else
    img->mem = dt_vkalloc(heap, mem_req.size, mem_req.alignment);
  // dt_log(s_log_pipe, "allocating %.1f/%.1f MB for %"PRItkn" %"PRItkn" "
  //     "%"PRItkn" %"PRItkn,
  //     mem_req.size/(1024.0*1024.0), size/(1024.0*1024.0), dt_token_str(node->name), dt_token_str(c->name),
  //     dt_token_str(c->chan), dt_token_str(c->format));
  assert(img->mem);
  img->offset = img->mem->offset + heap_offset;
  img->size   = img->mem->size;
  // reference counting. we can't just do a ref++ here because we will
  // free directly after and wouldn't know which node later on still relies
  // on this buffer. hence we ran a reference counting pass before this, and
  // init the reference counter now accordingly:
  if(heap_offset == 0)
    img->mem->ref = c->connected_mi;

  // TODO: better and more general caching:
  if(heap_offset == 0 && (c->type == dt_token("source") || (c->flags & s_conn_protected)))
    img->mem->ref++; // add one more so we can run the pipeline starting from after upload easily

  return VK_SUCCESS;
}

// allocate output buffers, also create vulkan pipeline and load spir-v portion
// of the compute shader.
static inline VkResult
alloc_outputs(dt_graph_t *graph, dt_node_t *node)
{
  const int f = graph->frame % 2;
  // create descriptor bindings and pipeline:
  // we'll bind our buffers in the same order as in the connectors file.
  uint32_t drawn_connector_cnt = 0;
  uint32_t drawn_connector[DT_MAX_CONNECTORS];
  VkDescriptorSetLayoutBinding bindings[DT_MAX_CONNECTORS] = {{0}};
  for(int i=0;i<node->num_connectors;i++)
  {
    bindings[i].binding = i;
    if(dt_connector_ssbo(node->connector+i))
    {
      graph->dset_cnt_buffer += MAX(1, node->connector[i].array_length);
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    else if(dt_connector_input(node->connector+i))
    {
      graph->dset_cnt_image_read += MAX(1, node->connector[i].array_length);
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    else
    {
      graph->dset_cnt_image_write += MAX(1, node->connector[i].array_length);
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      if(node->type == s_node_graphics)
        drawn_connector[drawn_connector_cnt++] = i;
    }
    bindings[i].descriptorCount = MAX(1, node->connector[i].array_length);
    bindings[i].stageFlags = VK_SHADER_STAGE_ALL;//COMPUTE_BIT;
    bindings[i].pImmutableSamplers = node->connector[i].format == dt_token("yuv") ?
      &qvk.tex_sampler_yuv : 0;
  }

  // create render pass in case we need rasterised output:
  if(drawn_connector_cnt)
  {
    VkAttachmentDescription attachment_desc[DT_MAX_CONNECTORS];
    VkAttachmentReference color_attachment[DT_MAX_CONNECTORS];
    for(int i=0;i<drawn_connector_cnt;i++)
    {
      int k = drawn_connector[i];
      attachment_desc[i] = (VkAttachmentDescription) {
        .format         = dt_connector_vkformat(node->connector+k),
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR, // VK_ATTACHMENT_LOAD_OP_DONT_CARE, // select on s_conn_clear flag?
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      };
      color_attachment[i] = (VkAttachmentReference) {
        .attachment = i,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      };
    }
    VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = drawn_connector_cnt,
      .pColorAttachments = color_attachment,
    };
    VkRenderPassCreateInfo info = {
      .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = drawn_connector_cnt,
      .pAttachments    = attachment_desc,
      .subpassCount    = 1,
      .pSubpasses      = &subpass,
    };
    QVK(vkCreateRenderPass(qvk.device, &info, 0, &node->draw_render_pass));
  }

  // a sink needs a descriptor set (for display via imgui)
  if(!dt_node_source(node))
  {
    // create a descriptor set layout
    VkDescriptorSetLayoutCreateInfo dset_layout_info = {
      .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = node->num_connectors,
      .pBindings    = bindings,
    };
    QVKR(vkCreateDescriptorSetLayout(qvk.device, &dset_layout_info, 0, &node->dset_layout));
  }

  // a sink or a source does not need a pipeline to be run.
  // note, however, that a sink does need a descriptor set, as we want to bind
  // it to imgui textures later on.
  if(!(dt_node_sink(node) || dt_node_source(node)))
  {
    // create the pipeline layout
    VkDescriptorSetLayout dset_layout[] = {
      graph->uniform_dset_layout,
      node->dset_layout,
      graph->rt[f].dset_layout, // goes last, is optional
    };
    VkPushConstantRange pcrange = {
      .stageFlags = VK_SHADER_STAGE_ALL,
      .offset     = 0,
      .size       = node->push_constant_size,
    };
    VkPipelineLayoutCreateInfo layout_info = {
      .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount         = LENGTH(dset_layout) - 1 + dt_raytrace_present(graph),
      .pSetLayouts            = dset_layout,
      .pushConstantRangeCount = node->push_constant_size ? 1 : 0,
      .pPushConstantRanges    = node->push_constant_size ? &pcrange : 0,
    };
    QVKR(vkCreatePipelineLayout(qvk.device, &layout_info, 0, &node->pipeline_layout));

    if(drawn_connector_cnt)
    { // create rasterisation pipeline
      // TODO: cache shader modules on global struct? (we'll reuse the draw kernels for all strokes)
      const int wd = node->connector[drawn_connector[0]].roi.wd;
      const int ht = node->connector[drawn_connector[0]].roi.ht;
      VkShaderModule shader_module_vert, shader_module_geom, shader_module_frag;
      QVKR(dt_graph_create_shader_module(graph, node->name, node->kernel, "vert", &shader_module_vert));
      QVKR(dt_graph_create_shader_module(graph, node->name, node->kernel, "frag", &shader_module_frag));
      VkResult geom = dt_graph_create_shader_module(graph, node->name, node->kernel, "geom", &shader_module_geom);

      // vertex shader, geometry shader, fragment shader
      VkPipelineShaderStageCreateInfo shader_info[] = {{
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_VERTEX_BIT,
          .module = shader_module_vert,
          .pName  = "main",
      },{
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = shader_module_frag,
          .pName  = "main",
      },{
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_GEOMETRY_BIT,
          .module = shader_module_geom,
          .pName  = "main",
      }};

      VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 0,
        .pVertexBindingDescriptions      = NULL,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions    = NULL,
      };

      VkPipelineInputAssemblyStateCreateInfo input_assembly_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        // TODO: make configurable:
        .topology               = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        .primitiveRestartEnable = VK_FALSE,
      };

      VkViewport viewport = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (float) wd,
        .height   = (float) ht,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
      };

      VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = {
          .width  = wd,
          .height = ht,
        },
      };

      VkPipelineViewportStateCreateInfo viewport_state = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports    = &viewport,
        .scissorCount  = 1,
        .pScissors     = &scissor,
      };

      VkPipelineRasterizationStateCreateInfo rasterizer_state = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable        = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE, /* skip rasterizer */
        .polygonMode             = VK_POLYGON_MODE_FILL,
        .lineWidth               = 1.0f,
        .cullMode                = VK_CULL_MODE_NONE,
        .frontFace               = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable         = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp          = 0.0f,
        .depthBiasSlopeFactor    = 0.0f,
      };

      VkPipelineMultisampleStateCreateInfo multisample_state = {
        .sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable   = VK_FALSE,
        .rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading      = 1.0f,
        .pSampleMask           = NULL,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable      = VK_FALSE,
      };

      VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT
          | VK_COLOR_COMPONENT_G_BIT
          | VK_COLOR_COMPONENT_B_BIT
          | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR,
        .dstColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR,
        .colorBlendOp        = VK_BLEND_OP_MAX,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp        = VK_BLEND_OP_MAX,
      };

      VkPipelineColorBlendStateCreateInfo color_blend_state = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable   = VK_FALSE,
        .logicOp         = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments    = &color_blend_attachment,
        .blendConstants  = { 0.0f, 0.0f, 0.0f, 0.0f },
      };

      VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType      = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = geom == VK_SUCCESS ? LENGTH(shader_info) : LENGTH(shader_info)-1,
        .pStages    = shader_info,

        .pVertexInputState   = &vertex_input_info,
        .pInputAssemblyState = &input_assembly_info,
        .pViewportState      = &viewport_state,
        .pRasterizationState = &rasterizer_state,
        .pMultisampleState   = &multisample_state,
        .pDepthStencilState  = NULL,
        .pColorBlendState    = &color_blend_state,
        .pDynamicState       = NULL,

        .layout              = node->pipeline_layout,
        .renderPass          = node->draw_render_pass,
        .subpass             = 0,

        .basePipelineHandle  = VK_NULL_HANDLE,
        .basePipelineIndex   = -1,
      };

      QVKR(vkCreateGraphicsPipelines(qvk.device, VK_NULL_HANDLE,
            1, &pipeline_info, NULL, &node->pipeline));

      // TODO: keep cached for others
      vkDestroyShaderModule(qvk.device, shader_module_vert, 0);
      vkDestroyShaderModule(qvk.device, shader_module_frag, 0);
      if(geom == VK_SUCCESS) vkDestroyShaderModule(qvk.device, shader_module_geom, 0);
    }
    else
    { // create the compute shader stage
      VkShaderModule shader_module;
      QVKR(dt_graph_create_shader_module(graph, node->name, node->kernel, "comp", &shader_module));

      // TODO: cache pipelines on module->so ?
      // VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT sub = {
      //   .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
      //   .requiredSubgroupSize = 32,
      // };
      VkPipelineShaderStageCreateInfo stage_info = {
        .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
        .pSpecializationInfo = 0,
        .pName               = "main", // arbitrary entry point symbols are supported by glslangValidator, but need extra compilation, too. i think it's easier to structure code via includes then.
        .module              = shader_module,
        // .pNext               = &sub,
      };

      // finally create the pipeline
      VkComputePipelineCreateInfo pipeline_info = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = stage_info,
        .layout = node->pipeline_layout
      };
      QVKR(vkCreateComputePipelines(qvk.device, VK_NULL_HANDLE, 1, &pipeline_info, 0, &node->pipeline));

      // we don't need the module any more
      vkDestroyShaderModule(qvk.device, stage_info.module, 0);
    }
  } // done with pipeline

  for(int i=0;i<node->num_connectors;i++)
  {
    dt_connector_t *c = node->connector+i;
    if(dt_connector_ssbo(c) && dt_connector_output(c))
    {
      // prepare allocation for storage buffer. this is easy, we do not
      // need to create an image, and computing the size is easy, too.
      const size_t size = dt_connector_bufsize(c, c->roi.wd, c->roi.ht);
      for(int f=0;f<c->frames;f++)
      for(int k=0;k<MAX(1,c->array_length);k++)
      {
        dt_connector_image_t *img = dt_graph_connector_image(graph,
            node - graph->node, i, k, f);
        if(img->image)  vkDestroyImage (qvk.device, img->image,  VK_NULL_HANDLE);
        if(img->buffer) vkDestroyBuffer(qvk.device, img->buffer, VK_NULL_HANDLE);
        img->image  = 0;
        img->buffer = 0;
        VkBufferCreateInfo create_info = {
          .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
          .size        = size,
          .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
                      // VK_BUFFER_USAGE_VERTEX_BUFFER_BIT for draw nodes maybe?
          .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        QVKR(vkCreateBuffer(qvk.device, &create_info, 0, &img->buffer));
#ifdef DEBUG_MARKERS
#ifdef QVK_ENABLE_VALIDATION
        char name[100];
        const char *dedup;
        snprintf(name, sizeof(name), "%"PRItkn"_%"PRItkn"_%"PRItkn"@%d", dt_token_str(node->module->name), dt_token_str(node->kernel), dt_token_str(c->name), f);
        dt_stringpool_get(&graph->debug_markers, name, strlen(name), 0, &dedup);
        VkDebugMarkerObjectNameInfoEXT name_info = {
          .sType = VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT,
          .object = (uint64_t) img->buffer,
          .objectType = VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT,
          .pObjectName = dedup,
        };
        qvkDebugMarkerSetObjectNameEXT(qvk.device, &name_info);
#endif
#endif
        // fprintf(stderr, "DEBUG allocating ssbo %"PRItkn":%"PRItkn":%"PRItkn":%"PRItkn" %zu = %d x %d\n", dt_token_str(node->name), dt_token_str(node->module->inst), dt_token_str(node->kernel), dt_token_str(c->name), size, c->roi.wd, c->roi.ht);
        VkMemoryRequirements buf_mem_req;

        if(c->type == dt_token("source"))
        { // source nodes are protected because we want to avoid re-transmit when possible.
          // also, they are allocated in host-visible staging memory
          vkGetBufferMemoryRequirements(qvk.device, img->buffer, &buf_mem_req);
          if(graph->memory_type_bits_staging != ~0 && buf_mem_req.memoryTypeBits != graph->memory_type_bits_staging)
            dt_log(s_log_qvk|s_log_err, "staging memory type bits don't match!");
          graph->memory_type_bits_staging = buf_mem_req.memoryTypeBits;

          img->mem = dt_vkalloc_feedback(&graph->heap_staging, buf_mem_req.size, buf_mem_req.alignment);
        }
        else
        { // other buffers are device only, i.e. use the default heap:
          vkGetBufferMemoryRequirements(qvk.device, img->buffer, &buf_mem_req);
          if(graph->memory_type_bits_ssbo != ~0 && buf_mem_req.memoryTypeBits != graph->memory_type_bits_ssbo)
          {
            dt_log(s_log_qvk|s_log_err, "node %"PRItkn" %"PRItkn" %"PRItkn":",
                dt_token_str(node->module->name), dt_token_str(node->kernel), dt_token_str(c->name));
            dt_log(s_log_qvk|s_log_err, "buffer memory type bits don't match! %d %d", buf_mem_req.memoryTypeBits, graph->memory_type_bits_ssbo);
          }
          graph->memory_type_bits_ssbo = buf_mem_req.memoryTypeBits;

          if((c->frames == 2 || (c->flags & s_conn_protected))) // allocate protected memory
            img->mem = dt_vkalloc_feedback(&graph->heap_ssbo, buf_mem_req.size, buf_mem_req.alignment);
          else
            img->mem = dt_vkalloc(&graph->heap_ssbo, buf_mem_req.size, buf_mem_req.alignment);
        }
        img->offset = img->mem->offset;
        img->size   = size; // for validation layers, this is the smaller of the two sizes.
        // set the staging offsets so it'll transparently work with read_source further down
        // when running the graph. this can cause trouble for multiple source ssbo in the
        // same node (as multiple places in the code e.g. using a single read_source call)
        c->offset_staging = img->mem->offset;
        c->size_staging   = size;
        // reference counting. we can't just do a ref++ here because we will
        // free directly after and wouldn't know which node later on still relies
        // on this buffer. hence we ran a reference counting pass before this, and
        // init the reference counter now accordingly:
        img->mem->ref = c->connected_mi;

        if(c->type == dt_token("source"))
          img->mem->ref++; // add one more so we can run the pipeline starting from after upload easily
      }
    }
    else if(dt_connector_ssbo(c) && dt_connector_input(c))
    { // point our inputs to their counterparts:
      if(c->connected_mi >= 0)
      {
        // point to conn_image of connected output directly:
        node->conn_image[i] = graph->node[c->connected_mi].conn_image[c->connected_mc];
        // TODO: as opposed to images, we do ideally not require staging
        // memory. in fact i'm too lazy to track whether there's a sink node
        // attached here, which would mean we would have had to allocate this
        // buffer in host-visible staging memory in the output branch above.
        // this means currently we would require a staging copy for ssbo sinks,
        // but it's not implemented.
      }
    }
    else if(!dt_connector_ssbo(c) && dt_connector_output(c))
    { // allocate our output buffers
      const int dynalloc = c->flags & s_conn_dynamic_array; // connector asks for their own memory pool
      if(c->array_alloc)
      { // free any potential residuals of dynamic allocation
        dt_vkalloc_cleanup(c->array_alloc);
        free(c->array_alloc);
        c->array_alloc = 0;
        c->array_alloc_size = 0;
        c->array_mem = 0; // freeing will not do anything, since it's protected. this will only go away via nuking/graph_reset
      }
      // if regular allocation takes place, do it now (dynamic will be deferred)
      if(!dynalloc)
        for(int f=0;f<c->frames;f++)
          for(int k=0;k<MAX(1,c->array_length);k++)
            QVKR(allocate_image_array_element(
                graph, node, c, &graph->heap, 0, f, k));
      else
      { // in case of dynamic allocation, reserve a protected block and wait until later
        c->array_mem = dt_vkalloc_feedback(&graph->heap, c->array_alloc_size, 0x10000); // this is shit and should probably get a fake alignment value for a fake image. amd requires this large one, nvidia can do one 0 less
        c->array_alloc = calloc(sizeof(dt_vkalloc_t), 1);
        dt_vkalloc_init(c->array_alloc, c->array_length * 2, c->array_alloc_size);
      }

      // allocate only one staging buffer for the whole array:
      if(c->type == dt_token("source"))
      {
        // allocate staging buffer for uploading to the just allocated image
        VkBufferCreateInfo buffer_info = {
          .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
          .size        = dt_connector_bufsize(c, c->roi.wd, c->roi.ht),
          .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        QVKR(vkCreateBuffer(qvk.device, &buffer_info, 0, &c->staging));
#ifdef DEBUG_MARKERS
#ifdef QVK_ENABLE_VALIDATION
        char name[100];
        const char *dedup;
        snprintf(name, sizeof(name), "%"PRItkn"_%"PRItkn"_%"PRItkn"@%d", dt_token_str(node->module->name), dt_token_str(node->kernel), dt_token_str(c->name), 0);
        dt_stringpool_get(&graph->debug_markers, name, strlen(name), 0, &dedup);
        VkDebugMarkerObjectNameInfoEXT name_info = {
          .sType       = VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT,
          .object      = (uint64_t) c->staging,
          .objectType  = VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT,
          .pObjectName = dedup,
        };
        qvkDebugMarkerSetObjectNameEXT(qvk.device, &name_info);
#endif
#endif
        VkMemoryRequirements buf_mem_req;
        vkGetBufferMemoryRequirements(qvk.device, c->staging, &buf_mem_req);
        if(graph->memory_type_bits_staging != ~0 && buf_mem_req.memoryTypeBits != graph->memory_type_bits_staging)
          dt_log(s_log_qvk|s_log_err, "staging memory type bits don't match!");
        graph->memory_type_bits_staging = buf_mem_req.memoryTypeBits;
        c->mem_staging    = dt_vkalloc(&graph->heap_staging, buf_mem_req.size, buf_mem_req.alignment);
        c->offset_staging = c->mem_staging->offset;
        c->size_staging   = c->mem_staging->size;
        c->mem_staging->ref++; // ref staging memory so we don't overwrite it before the pipe starts (will be written in read_source() in the module)
      }
    }
    else if(!dt_connector_ssbo(c) && dt_connector_input(c))
    { // point our inputs to their counterparts:
      if(c->connected_mi >= 0)
      {
        // point to conn_image of connected output directly:
        node->conn_image[i] = graph->node[c->connected_mi].conn_image[c->connected_mc];
        if(c->type == dt_token("sink"))
        {
          // allocate staging buffer for downloading from connected input
          VkBufferCreateInfo buffer_info = {
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = dt_connector_bufsize(c, c->roi.wd, c->roi.ht),
            .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
          };
          QVKR(vkCreateBuffer(qvk.device, &buffer_info, 0, &c->staging));
#ifdef DEBUG_MARKERS
#ifdef QVK_ENABLE_VALIDATION
          char name[100];
          const char *dedup;
          snprintf(name, sizeof(name), "%"PRItkn"_%"PRItkn"_%"PRItkn"@%d", dt_token_str(node->module->name), dt_token_str(node->kernel), dt_token_str(c->name), 0);
          dt_stringpool_get(&graph->debug_markers, name, strlen(name), 0, &dedup);
          VkDebugMarkerObjectNameInfoEXT name_info = {
            .sType       = VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT,
            .object      = (uint64_t)c->staging,
            .objectType  = VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT,
            .pObjectName = dedup,
          };
        qvkDebugMarkerSetObjectNameEXT(qvk.device, &name_info);
#endif
#endif

          VkMemoryRequirements buf_mem_req;
          vkGetBufferMemoryRequirements(qvk.device, c->staging, &buf_mem_req);
          if(graph->memory_type_bits_staging != ~0 && buf_mem_req.memoryTypeBits != graph->memory_type_bits_staging)
            dt_log(s_log_qvk|s_log_err, "staging memory type bits don't match!");
          graph->memory_type_bits_staging = buf_mem_req.memoryTypeBits;
          c->mem_staging    = dt_vkalloc(&graph->heap_staging, buf_mem_req.size, buf_mem_req.alignment);
          c->offset_staging = c->mem_staging->offset;
          c->size_staging   = c->mem_staging->size;
          c->mem_staging->ref++; // so we don't overwrite it before the pipe ends, because write_sink is called for all modules once at the end
        }
      }
    }
  }
  return VK_SUCCESS;
}

static inline VkResult
bind_buffers_to_memory(
    dt_graph_t     *graph,
    dt_node_t      *node,
    dt_connector_t *c,
    int             f,     // frame index
    int             k)     // array index
{
  if(!dt_connector_output(c)) return VK_SUCCESS;
  dt_connector_image_t *img = dt_graph_connector_image(graph, node - graph->node, c - node->connector, k, f);
  if(dt_connector_ssbo(c))
  { // storage buffer
    if(img->image_view) vkDestroyImageView(qvk.device, img->image_view, VK_NULL_HANDLE);
    img->image_view = 0;
    if(c->type == dt_token("source"))
      QVKR(vkBindBufferMemory(qvk.device, img->buffer, graph->vkmem_staging, img->offset));
    else
      QVKR(vkBindBufferMemory(qvk.device, img->buffer, graph->vkmem_ssbo, img->offset));
  }
  else
  { // storage image
    if(!img->mem) return VK_SUCCESS; // ignore freed slots for dynamically allocated arrays.

    // only actually backed by memory if we have all these frames.
    // else we'll just point to the same image for all frames.
    if(c->format == dt_token("yuv"))
    { // two-plane yuv:
      VkBindImagePlaneMemoryInfo bind_image_plane0_info = {
        .sType       = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
        .planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT,
      };
      VkBindImagePlaneMemoryInfo bind_image_plane1_info = {
        .sType       = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
        .planeAspect = VK_IMAGE_ASPECT_PLANE_1_BIT,
      };
      VkBindImageMemoryInfo bind_image_memory_infos[] = {{
        .sType        = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .pNext        = &bind_image_plane0_info,
        .image        = img->image,
        .memory       = graph->vkmem,
        .memoryOffset = img->offset,
      },{
        .sType        = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .pNext        = &bind_image_plane1_info,
        .image        = img->image,
        .memory       = graph->vkmem,
        .memoryOffset = img->offset + img->plane1_offset,
      }};
      QVKR(vkBindImageMemory2(qvk.device, 2, bind_image_memory_infos));
    }
    else
    { // plain:
      QVKR(vkBindImageMemory(qvk.device, img->image, graph->vkmem, img->offset));
    }

    VkSamplerYcbcrConversionInfo ycbcr_info = {
      .sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
      .conversion = qvk.yuv_conversion,
    };
    VkImageViewCreateInfo images_view_create_info = {
      .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext      = c->format == dt_token("yuv") ? &ycbcr_info : 0,
      .viewType   = VK_IMAGE_VIEW_TYPE_2D,
      .format     = dt_connector_vkformat(c),
      .image      = img->image,
      .subresourceRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1
      },
    };
    if(img->image_view) vkDestroyImageView(qvk.device, img->image_view, VK_NULL_HANDLE);
    QVKR(vkCreateImageView(qvk.device, &images_view_create_info, NULL, &img->image_view));
  }
  return VK_SUCCESS;
}

// 2nd pass, now we have images and vkDeviceMemory, let's bind images and buffers
// to memory and create descriptor sets
static inline VkResult
alloc_outputs2(dt_graph_t *graph, dt_node_t *node)
{
  for(int i=0;i<node->num_connectors;i++)
  {
    dt_connector_t *c = node->connector+i;
    if(!(c->flags & s_conn_dynamic_array)) // regular case, first pass
      for(int f=0;f<c->frames;f++) for(int k=0;k<MAX(1,c->array_length);k++)
        QVKR(bind_buffers_to_memory(graph, node, c, f, k));
  }
  return VK_SUCCESS;
}

// update descriptor sets
static inline void
write_descriptor_sets(
    dt_graph_t     *graph,
    dt_node_t      *node,
    dt_connector_t *c,
    int             dyn_array)
{
  VkDescriptorImageInfo  *img_info = alloca(sizeof(VkDescriptorImageInfo) *DT_GRAPH_MAX_FRAMES*MAX(1,c->array_length));
  VkDescriptorBufferInfo *buf_info = alloca(sizeof(VkDescriptorBufferInfo)*DT_GRAPH_MAX_FRAMES*MAX(1,c->array_length));
  VkWriteDescriptorSet    img_dset[DT_GRAPH_MAX_FRAMES] = {{0}};
  int cur_img = 0, cur_dset = 0, cur_buf = 0;

  if(dt_connector_output(c))
  { // allocate our output buffers
    if(!dyn_array && dt_connector_ssbo(c))
    { // storage buffer
      for(int f=0;f<DT_GRAPH_MAX_FRAMES;f++)
      {
        int ii = cur_buf;
        for(int k=0;k<MAX(1,c->array_length);k++)
        {
          dt_connector_image_t *img = dt_graph_connector_image(graph,
              node - graph->node, c - node->connector, k, MIN(f, c->frames-1));
          int iii = cur_buf++;
          buf_info[iii].buffer = img->buffer;
          buf_info[iii].offset = 0;
          buf_info[iii].range  = img->size;
        }
        int dset = cur_dset++;
        img_dset[dset].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        img_dset[dset].dstSet          = node->dset[f];
        img_dset[dset].dstBinding      = c - node->connector;
        img_dset[dset].dstArrayElement = 0;
        img_dset[dset].descriptorCount = MAX(c->array_length, 1);
        img_dset[dset].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        img_dset[dset].pBufferInfo     = buf_info + ii;
      }
    }
    else
    { // image buffer
      // dynamic arrays will only initialise the descriptor set necessary for the current frame
      // since the frames refer to the odd/even pipelines and not to feedback buffers used in the same
      // command buffer:
      const int c_dyn_array = c->flags & s_conn_dynamic_array;
      int fm = c_dyn_array ? (graph->frame % 2) : 0;
      int fM = c_dyn_array ? (graph->frame % 2) + 1 : DT_GRAPH_MAX_FRAMES;
      if((!dyn_array && !c_dyn_array) || (dyn_array && c_dyn_array)) for(int f=fm;f<fM;f++)
      {
        int ii = cur_img;
        for(int k=0;k<MAX(1,c->array_length);k++)
        {
          dt_connector_image_t *img = dt_graph_connector_image(graph,
              node - graph->node, c - node->connector, k, MIN(f, c->frames-1));
          if(!img->image_view) // for dynamic arrays maybe a lazy programmer left empty slots? set to tex 0:
            img = dt_graph_connector_image(graph,
                node - graph->node, c - node->connector, 0, MIN(f, c->frames-1));

          int iii = cur_img++;
          img_info[iii].sampler     = VK_NULL_HANDLE;
          img_info[iii].imageView   = img->image_view;
          assert(img->image_view);
          img_info[iii].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        int dset = cur_dset++;
        img_dset[dset].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        img_dset[dset].dstSet          = node->dset[f];
        img_dset[dset].dstBinding      = c - node->connector;
        img_dset[dset].dstArrayElement = 0;
        img_dset[dset].descriptorCount = MAX(c->array_length, 1);
        img_dset[dset].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        img_dset[dset].pImageInfo      = img_info + ii;
      }
    }
  }
  else if(dt_connector_input(c))
  { // point our inputs to their counterparts:
    const int c_dyn_array = (c->connected_mi >=0 && c->connected_mc >= 0) ? graph->node[c->connected_mi].connector[c->connected_mc].flags & s_conn_dynamic_array : 0;
    if(c->connected_mi >= 0 &&
      ((!dyn_array && !c_dyn_array) || (dyn_array && c_dyn_array)))
    {
      int fm = c_dyn_array ? (graph->frame % 2) : 0;
      int fM = c_dyn_array ? (graph->frame % 2) + 1 : DT_GRAPH_MAX_FRAMES;
      for(int f=fm;f<fM;f++)
      {
        int ii = dt_connector_ssbo(c) ? cur_buf : cur_img;
        for(int k=0;k<MAX(1,c->array_length);k++)
        {
          int frame = MIN(f, 
            graph->node[c->connected_mi].connector[c->connected_mc].frames-1);
          if(c->flags & s_conn_feedback)
          { // feedback connections cross the frame wires:
            frame = 1-f;
            // this should be ensured during connection:
            assert(c->frames == 2); 
            assert(graph->node[c->connected_mi].connector[c->connected_mc].frames == 2);
          }
          dt_connector_image_t *img  = dt_graph_connector_image(graph,
              node - graph->node, c - node->connector, k, frame);
          if(!img->image_view) // for dynamic arrays maybe a lazy programmer left empty slots? set to tex 0:
            img = dt_graph_connector_image(graph,
                node - graph->node, c - node->connector, 0, frame);
          if(dt_connector_ssbo(c))
          { // storage buffer
            int iii = cur_buf++;
            buf_info[iii].buffer = img->buffer;
            buf_info[iii].offset = 0;
            buf_info[iii].range  = img->size;
          }
          else
          { // image buffer
            // the image struct is shared between in and out connectors, but we 
            // acces the frame either straight or crossed, depending on feedback mode.
            int iii = cur_img++;
            if(c->format == dt_token("yuv"))
              img_info[iii].sampler   = 0; // needs immutable sampler
            else if(c->type == dt_token("sink") || c->format == dt_token("ui32"))
              img_info[iii].sampler   = qvk.tex_sampler_nearest;
            else
              img_info[iii].sampler   = qvk.tex_sampler;
            img_info[iii].imageView   = img->image_view;
            assert(img->image_view);
            img_info[iii].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          }
        }
        int dset = cur_dset++;
        img_dset[dset].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        img_dset[dset].dstSet          = node->dset[f];
        img_dset[dset].dstBinding      = c - node->connector;
        img_dset[dset].dstArrayElement = 0;
        img_dset[dset].descriptorCount = MAX(c->array_length, 1);
        if(dt_connector_ssbo(c))
        {
          img_dset[dset].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          img_dset[dset].pBufferInfo     = buf_info + ii;
        }
        else
        {
          img_dset[dset].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          img_dset[dset].pImageInfo      = img_info + ii;
        }
      }
    }
    else if(c->connected_mi < 0)
    { // sorry not connected, buffer will not be bound.
      // unconnected inputs are a problem however:
      dt_log(s_log_err | s_log_pipe, "kernel %"PRItkn"_%"PRItkn"_%"PRItkn":%"PRItkn" is not connected!",
          dt_token_str(node->name), dt_token_str(node->module->inst),
          dt_token_str(node->kernel), dt_token_str(c->name));
    }
  }
  if(node->dset_layout && cur_dset)
    vkUpdateDescriptorSets(qvk.device, cur_dset, img_dset, 0, NULL);
}

// initialise descriptor sets, bind staging buffer memory
static inline VkResult
alloc_outputs3(dt_graph_t *graph, dt_node_t *node)
{
  if(node->dset_layout)
  { // this is not set for source nodes. create descriptor set(s) for other nodes:
    VkDescriptorSetLayout lo[DT_GRAPH_MAX_FRAMES];
    for(int k=0;k<DT_GRAPH_MAX_FRAMES;k++) lo[k] = node->dset_layout;
    VkDescriptorSetAllocateInfo dset_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = graph->dset_pool,
      .descriptorSetCount = DT_GRAPH_MAX_FRAMES,
      .pSetLayouts = lo,
    };
    QVKR(vkAllocateDescriptorSets(qvk.device, &dset_info, node->dset));

    // uniform descriptor
    VkDescriptorSetAllocateInfo dset_info_u = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = graph->dset_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &graph->uniform_dset_layout,
    };
    QVKR(vkAllocateDescriptorSets(qvk.device, &dset_info_u, node->uniform_dset+0));
    QVKR(vkAllocateDescriptorSets(qvk.device, &dset_info_u, node->uniform_dset+1));
    VkDescriptorBufferInfo uniform_info[] = {{
      .buffer      = graph->uniform_buffer,
      .offset      = 0,
      .range       = graph->uniform_global_size,
    },{
      .buffer      = graph->uniform_buffer,
      .offset      = node->module->uniform_size ? node->module->uniform_offset : 0,
      .range       = node->module->uniform_size ? node->module->uniform_size : graph->uniform_global_size,
    },{
      .buffer      = graph->uniform_buffer,
      .offset      = graph->uniform_size,
      .range       = graph->uniform_global_size,
    },{
      .buffer      = graph->uniform_buffer,
      .offset      = graph->uniform_size + (node->module->uniform_size ? node->module->uniform_offset : 0),
      .range       = node->module->uniform_size ? node->module->uniform_size : graph->uniform_global_size,
    }};
    VkWriteDescriptorSet buf_dset[] = {{
      .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet          = node->uniform_dset[0],
      .dstBinding      = 0,
      .dstArrayElement = 0,
      .descriptorCount = 1,
      .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo     = uniform_info+0,
    },{
      .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet          = node->uniform_dset[0],
      .dstBinding      = 1,
      .dstArrayElement = 0,
      .descriptorCount = 1,
      .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo     = uniform_info+1,
    },{
      .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet          = node->uniform_dset[1],
      .dstBinding      = 0,
      .dstArrayElement = 0,
      .descriptorCount = 1,
      .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo     = uniform_info+2,
    },{
      .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet          = node->uniform_dset[1],
      .dstBinding      = 1,
      .dstArrayElement = 0,
      .descriptorCount = 1,
      .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo     = uniform_info+3,
    }};
    vkUpdateDescriptorSets(qvk.device, 4, buf_dset, 0, NULL);
  }

  for(int i=0;i<node->num_connectors;i++)
    if(!(node->connector[i].flags & s_conn_dynamic_array))
      write_descriptor_sets(graph, node, node->connector + i, 0);

  for(int i=0;i<node->num_connectors;i++)
  { // bind staging memory:
    dt_connector_t *c = node->connector+i;
    if(dt_connector_ssbo(c)) continue; // ssbo need no staging memory
    if(c->type == dt_token("source") || c->type == dt_token("sink"))
      vkBindBufferMemory(qvk.device, c->staging, graph->vkmem_staging, c->offset_staging);
  }

  if(node->type == s_node_graphics)
  { // create framebuffer for draw connectors
    VkImageView attachment[DT_MAX_CONNECTORS];
    int cnt = 0, first = -1;
    for(int i=0;i<node->num_connectors;i++) if(dt_connector_output(node->connector+i))
    { // make sure there is no array and make sure they are same res!
      if(first == -1) first = i;
      assert(node->connector[i].array_length <= 1);
      assert(node->connector[i].roi.wd == 
             node->connector[first].roi.wd);
      assert(node->connector[i].roi.ht == 
             node->connector[first].roi.ht);

      dt_connector_image_t *img  = dt_graph_connector_image(graph,
          node - graph->node, i, 0, 0);
      attachment[cnt++] = img->image_view;
    }
    VkFramebufferCreateInfo fb_create_info = {
      .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass      = node->draw_render_pass,
      .attachmentCount = cnt,
      .pAttachments    = attachment,
      .width           = node->connector[first].roi.wd,
      .height          = node->connector[first].roi.ht,
      .layers          = 1,
    };
    if(cnt) QVKR(vkCreateFramebuffer(qvk.device, &fb_create_info, NULL, &node->draw_framebuffer));
  }
  return VK_SUCCESS;
}

// free all buffers which we are done with now that the node
// has been processed. that is: all inputs and all of our outputs
// which aren't connected to another node.
// note that vkfree doesn't in fact free anything in case the reference count is > 0
static inline VkResult
free_inputs(dt_graph_t *graph, dt_node_t *node)
{
  for(int i=0;i<node->num_connectors;i++)
  {
    dt_connector_t *c = node->connector+i;
    if(c->type == dt_token("read") && c->connected_mi >= 0 &&
     !(c->flags & s_conn_feedback) &&
     !(graph->node[c->connected_mi].connector[c->connected_mc].flags & s_conn_dynamic_array))
    { // only free "read", not "sink" which we keep around for display
      // dt_log(s_log_pipe, "freeing input %"PRItkn"_%"PRItkn" %"PRItkn,
      //     dt_token_str(node->name),
      //     dt_token_str(node->kernel),
      //     dt_token_str(c->name));
      // ssbo do not use staging memory for sinks, and if they did these are no sinks:
      for(int f=0;f<c->frames;f++) for(int k=0;k<MAX(1,c->array_length);k++)
      {
        dt_connector_image_t *im = dt_graph_connector_image(graph, node-graph->node, i, k, f);
        if(!im) return VK_INCOMPLETE;
        if(dt_connector_ssbo(c)) dt_vkfree(&graph->heap_ssbo, im->mem);
        else                     dt_vkfree(&graph->heap,      im->mem);
      }
      // note that we keep the offset and VkImage etc around, we'll be using
      // these in consecutive runs through the pipeline and only clean up at
      // the very end. we just instruct our allocator that we're done with
      // this portion of the memory.
    }
    else if(dt_connector_output(c) && !(c->flags & s_conn_dynamic_array))
    {
      // dt_log(s_log_pipe, "freeing output ref count %"PRItkn"_%"PRItkn" %"PRItkn" %d %d",
      //     dt_token_str(node->name),
      //     dt_token_str(node->kernel),
      //     dt_token_str(c->name),
      //     c->connected_mi, c->mem->ref);
      for(int f=0;f<c->frames;f++) for(int k=0;k<MAX(1,c->array_length);k++)
      {
        dt_connector_image_t *im = dt_graph_connector_image(graph, node-graph->node, i, k, f);
        if(!im) return VK_INCOMPLETE;
        if(dt_connector_ssbo(c) && c->type == dt_token("source")) // host visible buffer
          dt_vkfree(&graph->heap_staging, im->mem);
        else // device visible only
        {
          if(dt_connector_ssbo(c)) dt_vkfree(&graph->heap_ssbo, im->mem);
          else                     dt_vkfree(&graph->heap,      im->mem);
        }
      }
    }
    // staging memory for sources or sinks only needed during execution once
    if(c->mem_staging)
    {
      // dt_log(s_log_pipe, "freeing staging %"PRItkn"_%"PRItkn" %"PRItkn,
      //     dt_token_str(node->name),
      //     dt_token_str(node->kernel),
      //     dt_token_str(c->name));
      dt_vkfree(&graph->heap_staging, c->mem_staging);
    }
  }
  return VK_SUCCESS;
}

// propagate full buffer size from source to sink
static void
modify_roi_out(dt_graph_t *graph, dt_module_t *module)
{
  int input = dt_module_get_connector(module, dt_token("input"));
  dt_connector_t *c = 0;
  if(input >= 0 &&
     graph->module[module->connector[input].connected_mi].img_param.input_name != dt_token("main"))
  for(int i=0;i<module->num_connectors;i++)
  { // the default main input is wherever "main" came in,
    // but preferrably named "input"
    if(!dt_connector_input(module->connector+i)) continue;
    int mid = module->connector[i].connected_mi;
    if(mid < 0) continue;
    if(graph->module[mid].img_param.input_name == dt_token("main"))
      input = i;
  }
  // main input modules have to init their params completely. we will not zero it.
  // this is because they run a modify_roi_out prepass to determine the main input
  // dimensions/types and we would destroy it here the second time around.
  if(module->inst != dt_token("main") || strncmp(dt_token_str(module->name), "i-", 2))
    module->img_param = (dt_image_params_t){{0}};
  if(input >= 0)
  { // first copy image metadata if we have a unique "input" connector
    c = module->connector + input;
    if(c->connected_mi != -1u)
      module->img_param = graph->module[c->connected_mi].img_param;
  }
  for(int i=0;i<module->num_connectors;i++)
  { // keep incoming roi in sync:
    dt_connector_t *c = module->connector+i;
    if(dt_connector_input(c) && c->connected_mi >= 0 && c->connected_mc >= 0)
      c->roi = graph->module[c->connected_mi].connector[c->connected_mc].roi;
  }
  if(!module->disabled && module->so->modify_roi_out)
  {
    module->so->modify_roi_out(graph, module);
    // mark roi in of all outputs as uninitialised:
    for(int i=0;i<module->num_connectors;i++)
      if(dt_connector_output(module->connector+i))
        module->connector[i].roi.scale = -1.0f;

    // if this is the main input of the graph, remember the img_param
    // globally, so others can pick up maker/model:
    if(module->inst == dt_token("main") && dt_connector_output(module->connector))
      graph->main_img_param = module->img_param;
    // remember source name to pass on further
    if(module->connector[0].type == dt_token("source"))
      module->img_param.input_name = module->inst;
  }
  else
  { // default implementation:
    // mark roi in of all outputs as uninitialised:
    for(int i=0;i<module->num_connectors;i++)
      if(dt_connector_output(module->connector+i))
        module->connector[i].roi.scale = -1.0f;
    // remember instance name if [0] is a source connector
    if(module->connector[0].type == dt_token("source"))
      module->img_param.input_name = module->inst;
    // copy over roi from connector named "input" to all outputs ("write")
    dt_roi_t roi = {0};
    if(input < 0)
    {
      // minimal default output size (for generator nodes with no callback)
      // might warn about this.
      roi.full_wd = 1024;
      roi.full_ht = 1024;
    }
    else
    {
      if(c->connected_mi != -1u)
      {
        roi = graph->module[c->connected_mi].connector[c->connected_mc].roi;
        c->roi = roi; // also keep incoming roi in sync
      }
    }
    for(int i=0;i<module->num_connectors;i++)
    {
      if(module->connector[i].type == dt_token("write"))
      {
        module->connector[i].roi.full_wd = roi.full_wd;
        module->connector[i].roi.full_ht = roi.full_ht;
      }
      if(module->connector[i].name == dt_token("dspy"))
      { // special temporary display modules have default roi of exactly what the gui needs:
        if(graph->gui_attached)
        {
          const float pwf = 0.2; // probably make a config param so we have access too (not only the gui)
          const float pwd = pwf * (16.0/9.0) * qvk.win_height;
          module->connector[i].roi.full_wd = 2.0/3.0 * pwd;
          module->connector[i].roi.full_ht = 2.0/3.0 * pwd;
        }
      }
    }
  }
}

// request input region of interest from sink to source
static void
modify_roi_in(dt_graph_t *graph, dt_module_t *module)
{
  if(!module->disabled && module->so->modify_roi_in)
  {
    module->so->modify_roi_in(graph, module);
  }
  else
  { // propagate roi request on output module to our inputs ("read")
    int output = dt_module_get_connector(module, dt_token("output"));
    if(output == -1 && module->connector[0].type == dt_token("sink"))
    { // by default ask for it all:
      output = 0;
      dt_roi_t *r = &module->connector[0].roi;
      r->scale = 1.0f;
      // this is the performance/LOD switch for faster processing
      // on low end computers. needs to be wired somehow in gui/config.
      if(module->connector[0].type == dt_token("sink") &&
         module->inst == dt_token("main"))
      { // scale to fit into requested roi
        float scalex = graph->output_wd > 0 ? r->full_wd / (float) graph->output_wd : 1.0f;
        float scaley = graph->output_ht > 0 ? r->full_ht / (float) graph->output_ht : 1.0f;
        r->scale = MAX(scalex, scaley);
      }
      r->wd = r->full_wd/r->scale;
      r->ht = r->full_ht/r->scale;
    }
    if(output < 0)
    {
      dt_log(s_log_pipe|s_log_err, "input roi on %"PRItkn"_%"PRItkn" uninitialised!",
          dt_token_str(module->name), dt_token_str(module->inst));
      return;
    }
    dt_roi_t *roi = &module->connector[output].roi;

    // all input connectors get the same roi as our output.
    for(int i=0;i<module->num_connectors;i++)
    {
      dt_connector_t *c = module->connector+i;
      if(dt_connector_input(c)) c->roi = *roi;
    }
  }

  // in any case copy over to output roi of connected modules:
  for(int i=0;i<module->num_connectors;i++)
  {
    dt_connector_t *c = module->connector+i;
    if(dt_connector_input(c))
    {
      // make sure roi is good on the outgoing connector
      if(c->connected_mi >= 0 && c->connected_mc >= 0)
      {
        dt_roi_t *roi = &graph->module[c->connected_mi].connector[c->connected_mc].roi;
        if(graph->module[c->connected_mi].connector[c->connected_mc].type == dt_token("source"))
        { // sources don't negotiate their size, they just give what they have
          roi->wd = roi->full_wd;
          roi->ht = roi->full_ht;
          if(roi->scale <= 0) roi->scale = 1.0; // mark as initialised, we force the resolution now
          c->roi = *roi; // TODO: this may mean that we need a resample node if the module can't handle it!
          // TODO: insert manually here
        }
        // if roi->scale > 0 it has been inited before and we're late to the party!
        // in this case, reverse the process:
        else if(roi->scale > 0.0f)
          c->roi = *roi; // TODO: this may mean that we need a resample node if the module can't handle it!
          // TODO: insert manually here
        else
          *roi = c->roi;
        // propagate flags:
        graph->module[c->connected_mi].connector[c->connected_mc].flags |= c->flags;
        // make sure we use the same array size as the data source. this is when the array_length depends on roi_out
        c->array_length = graph->module[c->connected_mi].connector[c->connected_mc].array_length;
      }
    }
  }
}

// convenience function for debugging in gdb:
void dt_token_print(dt_token_t t)
{
  fprintf(stderr, "token: %"PRItkn"\n", dt_token_str(t));
}

static VkResult
record_command_buffer(dt_graph_t *graph, dt_node_t *node, int runflag)
{
  VkCommandBuffer cmd_buf = graph->command_buffer[graph->frame % 2];

  // sanity check: are all input connectors bound?
  for(int i=0;i<node->num_connectors;i++)
    if(dt_connector_input(node->connector+i))
      if(node->connector[i].connected_mi == -1)
      {
        dt_log(s_log_err, "input %"PRItkn":%"PRItkn" not connected!",
            dt_token_str(node->name), dt_token_str(node->connector[i].name));
        return VK_INCOMPLETE;
      }

  // runflag will be 1 if we ask to upload source explicitly (the first time around)
  if((runflag == 0) && dt_node_source(node))
  {
    for(int i=0;i<node->num_connectors;i++)
    { // this is completely retarded and just to make the layout match what we expect below
      if(!dt_connector_ssbo(node->connector+i) &&
          dt_connector_output(node->connector+i) &&
         !(node->connector[i].flags & s_conn_dynamic_array))
      {
        if(node->type == s_node_graphics)
          IMG_LAYOUT(
              dt_graph_connector_image(graph, node-graph->node, i, 0, graph->frame),
              SHADER_READ_ONLY_OPTIMAL,
              COLOR_ATTACHMENT_OPTIMAL);
        else for(int k=0;k<MAX(node->connector[i].array_length,1);k++)
          IMG_LAYOUT(
              dt_graph_connector_image(graph, node-graph->node, i, k, graph->frame),
              SHADER_READ_ONLY_OPTIMAL,
              GENERAL);
      }
    }
    return VK_SUCCESS;
  }
  // TODO: extend the runflag to only switch on modules *after* cached input/changed parameters

  // special case for end of pipeline and thumbnail creation:
  if(graph->thumbnail_image &&
      node->name         == dt_token("thumb") &&
      node->module->inst == dt_token("main"))
  {
    VkImageCopy cp = {
      .srcSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
      },
      .srcOffset = {0},
      .dstSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
      },
      .dstOffset = {0},
      .extent = {
        .width  = node->connector[0].roi.wd,
        .height = node->connector[0].roi.ht,
        .depth  = 1,
      },
    };
    dt_connector_image_t *img = dt_graph_connector_image(graph,
        node-graph->node, 0, 0, 0);
    IMG_LAYOUT(img, UNDEFINED, TRANSFER_SRC_OPTIMAL);
    BARRIER_IMG_LAYOUT(graph->thumbnail_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cmd_buf,
        img->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        graph->thumbnail_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &cp);
    BARRIER_IMG_LAYOUT(graph->thumbnail_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return VK_SUCCESS;
  }

  // barriers and image layout transformations:
  // wait for our input images and transfer them to read only.
  // also wait for our output buffers to be transferred into general layout.
  // output is moved from UNDEFINED in the hope that the content can be discarded for
  // increased efficiency in this case.
  for(int i=0;i<node->num_connectors;i++)
  {
    // ssbo are not depending on image layouts and are also not cleared to zero:
    if(dt_connector_ssbo(node->connector+i))
    { // input buffers have a barrier
      if(dt_connector_input(node->connector+i))
        BARRIER_COMPUTE_BUFFER(
              dt_graph_connector_image(graph, node-graph->node, i, 0,
                (node->connector[i].flags & s_conn_feedback) ?
                1-(graph->frame & 1) :
                graph->frame)->buffer);
      if(dt_connector_output(node->connector+i) && (node->connector[i].flags & s_conn_clear))
      {
        VkBuffer buf = dt_graph_connector_image(graph, node-graph->node, i, 0,
                (node->connector[i].flags & s_conn_feedback) ?
                1-(graph->frame & 1) :
                graph->frame)->buffer;
        vkCmdFillBuffer(cmd_buf, buf, 0, VK_WHOLE_SIZE, 0);
        BARRIER_COMPUTE_BUFFER(buf);
      }
      continue;
    }
    if(dt_connector_input(node->connector+i))
    {
      // this needs to prepare the frame we're actually reading.
      // for feedback connections, this is crossed over.
      if(!((node->connector[i].type == dt_token("sink")) && node->module->so->write_sink))
      {
        if(!(node->connector[i].flags & s_conn_dynamic_array))
          for(int k=0;k<MAX(1,node->connector[i].array_length);k++)
            IMG_LAYOUT(
              dt_graph_connector_image(graph, node-graph->node, i, k,
                (node->connector[i].flags & s_conn_feedback) ?
                1-(graph->frame & 1) :
                graph->frame),
              GENERAL,
              SHADER_READ_ONLY_OPTIMAL);
      }
      else
        IMG_LAYOUT(
            dt_graph_connector_image(graph, node-graph->node, i, 0, graph->frame),
            GENERAL,
            TRANSFER_SRC_OPTIMAL);
    }
    else if(dt_connector_output(node->connector+i))
    {
      // wait for layout transition on the output back to general:
      if(node->connector[i].flags & s_conn_clear)
      {
        IMG_LAYOUT(
            dt_graph_connector_image(graph, node-graph->node, i, 0, graph->frame),
            UNDEFINED,//SHADER_READ_ONLY_OPTIMAL,
            TRANSFER_DST_OPTIMAL);
        // in case clearing is requested, zero out the image:
        VkClearColorValue col = {{0}};
        VkImageSubresourceRange range = {
          .aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT,
          .baseMipLevel    = 0,
          .levelCount      = 1,
          .baseArrayLayer  = 0,
          .layerCount      = 1
        };
        vkCmdClearColorImage(
            cmd_buf,
            dt_graph_connector_image(graph, node-graph->node, i, 0, graph->frame)->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &col,
            1,
            &range);
        if(node->type == s_node_graphics)
          IMG_LAYOUT(
              dt_graph_connector_image(graph, node-graph->node, i, 0, graph->frame),
              TRANSFER_DST_OPTIMAL,
              COLOR_ATTACHMENT_OPTIMAL);
        else for(int k=0;k<MAX(node->connector[i].array_length,1);k++)
          IMG_LAYOUT(
              dt_graph_connector_image(graph, node-graph->node, i, k, graph->frame),
              TRANSFER_DST_OPTIMAL,
              GENERAL);
      }
      else
      {
        if(node->type == s_node_graphics)
          IMG_LAYOUT(
              dt_graph_connector_image(graph, node-graph->node, i, 0, graph->frame),
              UNDEFINED,//SHADER_READ_ONLY_OPTIMAL,
              COLOR_ATTACHMENT_OPTIMAL);
        else if(!(node->connector[i].flags & s_conn_dynamic_array)) for(int k=0;k<MAX(node->connector[i].array_length,1);k++)
          IMG_LAYOUT(
              dt_graph_connector_image(graph, node-graph->node, i, k, graph->frame),
              UNDEFINED,//SHADER_READ_ONLY_OPTIMAL,
              GENERAL);
      }
    }
  }

  const uint32_t wd = node->connector[0].roi.wd;
  const uint32_t ht = node->connector[0].roi.ht;
  VkBufferImageCopy regions[] = {{
    .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .imageSubresource.layerCount = 1,
    .imageExtent = { wd, ht, 1 },
  },{
    .imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
    .imageSubresource.layerCount = 1,
    .imageExtent = { wd, ht, 1 },
  },{
    .bufferOffset = dt_graph_connector_image(graph, node-graph->node, 0, 0, graph->frame)->plane1_offset,
    .imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
    .imageSubresource.layerCount = 1,
    .imageExtent = { wd / 2, ht / 2, 1 },
  }};
  const int f = graph->frame % 2;
  const int yuv = node->connector[0].format == dt_token("yuv");
  if(dt_node_sink(node) && node->module->so->write_sink)
  { // only schedule copy back if the node actually asks for it
    if(dt_connector_ssbo(node->connector+0))
    {
      // TODO: ssbo buffer copy?
      // ssbo don't need staging memory, if we chose it host visible (currently only source ssbo)
    }
    else
    {
      vkCmdCopyImageToBuffer(
          cmd_buf,
          dt_graph_connector_image(graph, node-graph->node, 0, 0, graph->frame)->image,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          node->connector[0].staging,
          yuv ? 2 : 1, yuv ? regions+1 : regions);
      BARRIER_COMPUTE_BUFFER(node->connector[0].staging);
    }
  }
  else if(dt_node_source(node) &&
         !dt_connector_ssbo(node->connector+0) && // ssbo source nodes use staging memory and thus don't need a copy.
         (node->connector[0].array_length <= 1))  // arrays share the staging buffer, are handled by iterating read_source()
  {
    // push profiler start
    if(graph->query[f].cnt < graph->query[f].max)
    {
      vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          graph->query[f].pool, graph->query[f].cnt);
      graph->query[f].name  [graph->query[f].cnt  ] = node->name;
      graph->query[f].kernel[graph->query[f].cnt++] = node->kernel;
    }
    IMG_LAYOUT(
        dt_graph_connector_image(graph, node-graph->node, 0, 0, graph->frame),
        UNDEFINED,
        TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(
        cmd_buf,
        node->connector[0].staging,
        dt_graph_connector_image(graph, node-graph->node, 0, 0, graph->frame)->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        yuv ? 2 : 1, yuv ? regions+1 : regions);
    IMG_LAYOUT(
        dt_graph_connector_image(graph, node-graph->node, 0, 0, graph->frame),
        TRANSFER_DST_OPTIMAL,
        SHADER_READ_ONLY_OPTIMAL);
    // get a profiler timestamp:
    if(graph->query[f].cnt < graph->query[f].max)
    {
      vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          graph->query[f].pool, graph->query[f].cnt);
      graph->query[f].name  [graph->query[f].cnt  ] = node->name;
      graph->query[f].kernel[graph->query[f].cnt++] = node->kernel;
    }
  }

  // only non-sink and non-source nodes have a pipeline:
  if(!node->pipeline) return VK_SUCCESS;

  // push profiler start
  if(graph->query[f].cnt < graph->query[f].max)
  {
    vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        graph->query[f].pool, graph->query[f].cnt);
    graph->query[f].name  [graph->query[f].cnt  ] = node->name;
    graph->query[f].kernel[graph->query[f].cnt++] = node->kernel;
  }

  // compute or graphics pipeline?
  int draw = -1;
  if(node->type == s_node_graphics)
    for(int k=0;k<node->num_connectors;k++)
      if(dt_connector_output(node->connector+k)) { draw = k; break; }

  if(draw != -1)
  {
    VkClearValue clear_color = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo render_pass_info = {
      .sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass        = node->draw_render_pass,
      .framebuffer       = node->draw_framebuffer,
      .renderArea.offset = { 0, 0 },
      .renderArea.extent = {
        .width  = node->connector[draw].roi.wd,
        .height = node->connector[draw].roi.ht },
      .clearValueCount   = 1,
      .pClearValues      = &clear_color
    };
    vkCmdBeginRenderPass(cmd_buf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
  }

  // combine all descriptor sets:
  VkDescriptorSet desc_sets[] = {
    node->uniform_dset[f],
    node->dset[f],
    graph->rt[f].dset,
  };
  const int desc_sets_cnt = LENGTH(desc_sets) - 1 + dt_raytrace_present(graph);

  if(draw != -1)
  {
    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, node->pipeline);
    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
      node->pipeline_layout, 0, desc_sets_cnt, desc_sets, 0, 0);
  }
  else
  {
    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, node->pipeline);
    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
      node->pipeline_layout, 0, desc_sets_cnt, desc_sets, 0, 0);
  }

  // update some buffers:
  if(node->push_constant_size)
    vkCmdPushConstants(cmd_buf, node->pipeline_layout,
        VK_SHADER_STAGE_ALL, 0, node->push_constant_size, node->push_constant);

  if(draw == -1)
  {
    vkCmdDispatch(cmd_buf,
        (node->wd + DT_LOCAL_SIZE_X - 1) / DT_LOCAL_SIZE_X,
        (node->ht + DT_LOCAL_SIZE_Y - 1) / DT_LOCAL_SIZE_Y,
         node->dp);
  }
  else
  {
    const int pi = dt_module_get_param(node->module->so, dt_token("draw"));
    const int32_t *p_draw = dt_module_param_int(node->module, pi);

    if(p_draw[0] > 0)
      vkCmdDraw(cmd_buf, p_draw[0], 1, 0, 0);
    vkCmdEndRenderPass(cmd_buf);
  }

  // get a profiler timestamp:
  if(graph->query[f].cnt < graph->query[f].max)
  {
    vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        graph->query[f].pool, graph->query[f].cnt);
    graph->query[f].name  [graph->query[f].cnt  ] = node->name;
    graph->query[f].kernel[graph->query[f].cnt++] = node->kernel;
  }
  return VK_SUCCESS;
}

static void
count_references(dt_graph_t *graph, dt_node_t *node)
{
  // dt_log(s_log_pipe, "counting references for %"PRItkn"_%"PRItkn,
  //     dt_token_str(node->name),
  //     dt_token_str(node->kernel));
  for(int c=0;c<node->num_connectors;c++)
  {
    if(dt_connector_input(node->connector+c))
    {
      // up reference counter of the connector that owns the output
      int mi = node->connector[c].connected_mi;
      int mc = node->connector[c].connected_mc;
      if(mi != -1) graph->node[mi].connector[mc].connected_mi++;
      // dt_log(s_log_pipe, "references %d on output %"PRItkn"_%"PRItkn":%"PRItkn,
      //     graph->node[mi].connector[mc].connected_mi,
      //     dt_token_str(graph->node[mi].name),
      //     dt_token_str(graph->node[mi].kernel),
      //     dt_token_str(graph->node[mi].connector[mc].name));
    }
    else
    {
      // output cannot know all modules connected to it, so
      // it stores the reference counter instead.
      // we increase it here because every node needs its own outputs for processing.
      node->connector[c].connected_mi++;
      // dt_log(s_log_pipe, "references %d on output %"PRItkn"_%"PRItkn":%"PRItkn,
      //     node->connector[c].connected_mi,
      //     dt_token_str(node->name),
      //     dt_token_str(node->kernel),
      //     dt_token_str(node->connector[c].name));
    }
  }
}

// default callback for create nodes: pretty much copy the module.
// does no vulkan work, just graph connections. shall not fail.
static void
create_nodes(dt_graph_t *graph, dt_module_t *module, uint64_t *uniform_offset)
{
  for(int i=0;i<module->num_connectors;i++)
    module->connector[i].bypass_mi =
      module->connector[i].bypass_mc = -1;
  const uint64_t u_offset = *uniform_offset;
  uint64_t u_size = module->committed_param_size ?
    module->committed_param_size :
    module->param_size;
  u_size = (u_size + qvk.uniform_alignment-1) & -qvk.uniform_alignment;
  *uniform_offset += u_size;
  const int nodes_begin = graph->num_nodes;
  // TODO: if roi size/scale does not match, insert resample node!
  // TODO: where? inside create_nodes? or we fix it afterwards?
  if(module->disabled)
  {
    int mc_in = -1, mc_out = -1;
    for(int i=0;i<module->num_connectors;i++)
    {
      if(dt_connector_output(module->connector+i) && module->connector[i].name == dt_token("output"))
        mc_out = i;
      if(dt_connector_input(module->connector+i) && module->connector[i].name == dt_token("input"))
        mc_in = i;
    }
    // TODO error handling?
    dt_connector_bypass(graph, module, mc_in, mc_out);
  }
  else if(module->so->create_nodes)
  {
    module->so->create_nodes(graph, module);
  }
  else
  {
    assert(graph->num_nodes < graph->max_nodes);
    const int nodeid = graph->num_nodes++;
    dt_node_t *node = graph->node + nodeid;

    *node = (dt_node_t) {
      .name           = module->name,
      .kernel         = dt_token("main"), // file name
      .num_connectors = module->num_connectors,
      .module         = module,
      .flags          = module->flags,    // propagate sink/source copy requests
      // make sure we don't follow garbage pointers. pure sink or source nodes
      // don't run a pipeline. and hence have no descriptor sets to construct. they
      // do, however, have input or output images allocated for them.
      .pipeline       = 0,
      .dset_layout    = 0,
    };

    // compute shader or graphics pipeline?
    char filename[PATH_MAX+100] = {0};
    snprintf(filename, sizeof(filename), "%s/modules/%"PRItkn"/main.vert.spv",
        dt_pipe.basedir, dt_token_str(module->name));
    struct stat statbuf;
    if(stat(filename, &statbuf)) node->type = s_node_compute;
    else node->type = s_node_graphics;

    // determine kernel dimensions:
    int output = dt_module_get_connector(module, dt_token("output"));
    // output == -1 means we must be a sink, anyways there won't be any more glsl
    // to be run here!
    if(output >= 0)
    {
      dt_roi_t *roi = &module->connector[output].roi;
      node->wd = roi->wd;
      node->ht = roi->ht;
      node->dp = 1;
    }

    for(int i=0;i<module->num_connectors;i++)
      dt_connector_copy(graph, module, i, nodeid, i);
  }

  module->uniform_offset = u_offset;
  module->uniform_size   = u_size;
  // now init frame count, repoint connection image storage
  for(int n=nodes_begin;n<graph->num_nodes;n++)
  {
    for(int i=0;i<graph->node[n].num_connectors;i++)
    {
      if(graph->node[n].connector[i].frames == 0)
      {
        if(graph->node[n].connector[i].flags & s_conn_feedback)
          graph->node[n].connector[i].frames = 2;
        else
          graph->node[n].connector[i].frames = 1;
      }
      if(dt_connector_output(graph->node[n].connector+i))
      { // only allocate memory for output connectors
        graph->node[n].conn_image[i] = graph->conn_image_end;
        graph->conn_image_end += MAX(1,graph->node[n].connector[i].array_length)
          * graph->node[n].connector[i].frames;
        assert(graph->conn_image_end <= graph->conn_image_max);
      }
      else graph->node[n].conn_image[i] = -1;
    }
  }
}

VkResult dt_graph_run(
    dt_graph_t     *graph,
    dt_graph_run_t  run)
{
  double clock_beg = dt_time();
  dt_module_flags_t module_flags = 0;
  const int f  = graph->frame % 2;     // recording this pipeline now
  const int fp = (graph->frame+1) % 2; // waiting for the previous frame

  if(run & s_graph_run_alloc)
    QVKLR(&qvk.queue_mutex, vkDeviceWaitIdle(qvk.device));

  QVKR(vkWaitForFences(qvk.device, 1, &graph->command_fence[f], VK_TRUE, ((uint64_t)1)<<40)); // wait for last invocation of our command buffer, just in case

{ // module scope
  // find list of modules in post order
  uint32_t modid[100];
  assert(sizeof(modid)/sizeof(modid[0]) >= graph->num_modules);

  int cnt = 0;
  dt_module_t *const arr = graph->module;
  const int arr_cnt = graph->num_modules;
#define TRAVERSE_POST \
  modid[cnt++] = curr;
#include "graph-traverse.inc"

  int main_input_module = -1;
  // find extra module flags
  for(int i=0;i<cnt;i++)
  {
    if(!strncmp(dt_token_str(graph->module[modid[i]].name), "i-", 2) &&
       (main_input_module == -1 || graph->module[modid[i]].inst == dt_token("main")))
      main_input_module = modid[i];
    module_flags |= graph->module[modid[i]].flags;
  }

  // at least one module requested a full rebuild:
  if(module_flags & s_module_request_all) run |= s_graph_run_all;

  // if synchronous upload/download is required, we can't interleave frames:
  if((run & (s_graph_run_upload_source | s_graph_run_download_sink)) ||
     (module_flags & (s_module_request_read_source | s_module_request_write_sink)))
    run |= s_graph_run_wait_done;

  // only waiting for the gui thread to draw our output, and only
  // if we intend to clean it up behind their back
  if(graph->gui_attached &&
    (run & (s_graph_run_alloc | s_graph_run_create_nodes)))
    QVKLR(&qvk.queue_mutex, vkDeviceWaitIdle(qvk.device));

  if(run & s_graph_run_alloc)
  {
    vkDestroyDescriptorSetLayout(qvk.device, graph->uniform_dset_layout, 0);
    graph->uniform_dset_layout = 0;
    if(!graph->uniform_dset_layout)
    { // init layout of uniform descriptor set:
      VkDescriptorSetLayoutBinding bindings[] = {{
        .binding = 0, // global uniform, frame number etc
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_ALL,
      },{
        .binding = 1, // module local uniform, params struct
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_ALL,
      }};
      VkDescriptorSetLayoutCreateInfo dset_layout_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings    = bindings,
      };
      QVKR(vkCreateDescriptorSetLayout(qvk.device, &dset_layout_info, 0, &graph->uniform_dset_layout));
    }
  }

  // ==============================================
  // first pass: find output rois
  // ==============================================
  // execute after all inputs have been traversed:
  // "int curr" will be the current node
  // walk all inputs and determine roi on all outputs
  if(run & s_graph_run_roi)
  {
    if(main_input_module >= 0) // may set metadata required by others (such as find the right lut)
      modify_roi_out(graph, graph->module + main_input_module);
    for(int i=0;i<cnt;i++)
      modify_roi_out(graph, graph->module + modid[i]);
    for(int i=0;i<cnt;i++) // potentially init remaining feedback rois:
      if(graph->module[modid[i]].connector[0].roi.full_wd == 0)
        modify_roi_out(graph, graph->module + modid[i]);
  }


  // we want to make sure the last output/display is initialised.
  // if the roi out is 0, probably reading some sources went wrong and we need
  // to abort right here!
  for(int i=cnt-1;i>=0;i--)
  {
    if(graph->module[modid[i]].connector[0].type == dt_token("sink"))
    {
      if(graph->module[modid[i]].connector[0].roi.full_wd == 0) return VK_INCOMPLETE;
      break; // we're good
    }
  }


  // now we don't always want the full size buffer but are interested in a
  // scaled or cropped sub-region. actually this step is performed
  // transparently in the sink module's modify_roi_in first thing in the
  // second pass.

  // ==============================================
  // 2nd pass: request input rois
  // and create nodes for all modules
  // ==============================================
  if(run & (s_graph_run_roi | s_graph_run_create_nodes))
  { // delete all previous nodes
    QVKLR(&qvk.queue_mutex, vkDeviceWaitIdle(qvk.device));
    for(int i=0;i<graph->num_nodes;i++) for(int j=0;j<graph->node[i].num_connectors;j++)
      if(graph->node[i].connector[j].flags & s_conn_dynamic_array)
      { // free potential double images for multi-frame dsets
        for(int k=0;k<MAX(1,graph->node[i].connector[j].array_length);k++)
        { // avoid error message in case image does not exist:
          dt_connector_image_t *img0 = (graph->node[i].conn_image[j] != -1) ? dt_graph_connector_image(graph, i, j, k, 0) : 0;
          dt_connector_image_t *img1 = (graph->node[i].conn_image[j] != -1) ? dt_graph_connector_image(graph, i, j, k, 1) : 0;
          if(!img0 || !img1) continue;
          if(img0->image == img1->image)
          { // it's enough to clean up one replicant, the rest will shut down cleanly later:
            *img0 = (dt_connector_image_t){0};
          }
        }
      }
    for(int i=0;i<graph->conn_image_end;i++)
    {
      if(graph->conn_image_pool[i].buffer)     vkDestroyBuffer(qvk.device,    graph->conn_image_pool[i].buffer, VK_NULL_HANDLE);
      if(graph->conn_image_pool[i].image)      vkDestroyImage(qvk.device,     graph->conn_image_pool[i].image, VK_NULL_HANDLE);
      if(graph->conn_image_pool[i].image_view) vkDestroyImageView(qvk.device, graph->conn_image_pool[i].image_view, VK_NULL_HANDLE);
      graph->conn_image_pool[i].buffer = 0;
      graph->conn_image_pool[i].image = 0;
      graph->conn_image_pool[i].image_view = 0;
    }
    graph->conn_image_end = 0;
    for(int i=0;i<cnt;i++)
      for(int j=0;j<graph->module[modid[i]].num_connectors;j++)
        graph->module[modid[i]].connector[j].associated_i =
          graph->module[modid[i]].connector[j].associated_c = -1;
    for(int i=0;i<graph->num_nodes;i++)
    {
      for(int j=0;j<graph->node[i].num_connectors;j++)
      {
        dt_connector_t *c = graph->node[i].connector+j;
        c->associated_i = c->associated_c = -1;
        if(c->staging) vkDestroyBuffer(qvk.device, c->staging, VK_NULL_HANDLE);
        c->staging = 0;
      }
      vkDestroyPipelineLayout     (qvk.device, graph->node[i].pipeline_layout,  0);
      vkDestroyPipeline           (qvk.device, graph->node[i].pipeline,         0);
      vkDestroyDescriptorSetLayout(qvk.device, graph->node[i].dset_layout,      0);
      vkDestroyFramebuffer        (qvk.device, graph->node[i].draw_framebuffer, 0);
      vkDestroyRenderPass         (qvk.device, graph->node[i].draw_render_pass, 0);
      graph->node[i].pipeline_layout = 0;
      graph->node[i].pipeline = 0;
      graph->node[i].dset_layout = 0;
      graph->node[i].draw_framebuffer = 0;
      graph->node[i].draw_render_pass = 0;
      dt_raytrace_node_cleanup    (graph->node + i);
    }
    dt_raytrace_graph_cleanup(graph);
    graph->num_nodes = 0;
    // we need two uint32, alignment is 64 bytes
    graph->uniform_global_size = qvk.uniform_alignment; // global data, aligned
    uint64_t uniform_offset = graph->uniform_global_size;
    // skip modules with uninited roi! (these are disconnected/dead code elimination cases)
    for(int i=cnt-1;i>=0;i--)
      if(graph->module[modid[i]].connector[0].roi.full_wd > 0)
        modify_roi_in(graph, graph->module+modid[i]);
    for(int i=0;i<cnt;i++)
      if(graph->module[modid[i]].connector[0].roi.full_wd > 0)
        create_nodes(graph, graph->module+modid[i], &uniform_offset);
    // make sure connectors are zero inited:
    memset(graph->conn_image_pool, 0, sizeof(dt_connector_image_t)*graph->conn_image_end);
    graph->uniform_size = uniform_offset;

    // these feedback connectors really cause a lot of trouble.
    // we need to initialise the input connectors now after full graph traversal.
    // or else the connection performed during node creation will copy some node id
    // from the connected module that has not yet been initialised.
    // we work around this by storing the *module* id and connector in
    // dt_connector_copy() for feedback connectors, and repoint it here.
    // i suppose we could always do that, not only for feedback connectors, to
    // simplify control logic.
    for(int ni=0;ni<graph->num_nodes;ni++)
    {
      dt_node_t *n = graph->node + ni;
      for(int i=0;i<n->num_connectors;i++)
      {
        if(n->connector[i].associated_i >= 0) // needs repointing
        {
          int n0, c0;
          int mi0 = n->connector[i].associated_i;
          int mc0 = n->connector[i].associated_c;
          if(dt_connector_input(n->connector+i))
          { // walk node->module->module->node
            int mi1 = graph->module[mi0].connector[mc0].connected_mi;
            int mc1 = graph->module[mi0].connector[mc0].connected_mc;
            if(mi1 == -1u) continue;
            // check for a bypass chain
            // m3(out) -> m2(in) -> bypass m1(out) -> m0(in)
            if(graph->module[mi1].connector[mc1].bypass_mi >= 0)
            { // now go from mi1/mc1(out) -> m2 = bypass(in) -> m3 = conn(out)
              int mi2 = graph->module[mi1].connector[mc1].bypass_mi;
              int mc2 = graph->module[mi1].connector[mc1].bypass_mc;
              if(mi2 == -1u) continue;
              int mi3 = graph->module[mi2].connector[mc2].connected_mi;
              int mc3 = graph->module[mi2].connector[mc2].connected_mc;
              if(mi3 == -1u) continue;
              n0 = graph->module[mi3].connector[mc3].associated_i;
              c0 = graph->module[mi3].connector[mc3].associated_c;
            }
            else
            {
              n0 = graph->module[mi1].connector[mc1].associated_i;
              c0 = graph->module[mi1].connector[mc1].associated_c;
            }
          }
          else // if(dt_connector_output(n->connector+i))
          { // walk node->module->node
            n0 = graph->module[mi0].connector[mc0].associated_i;
            c0 = graph->module[mi0].connector[mc0].associated_c;
          }
          n->connector[i].connected_mi = n0;
          n->connector[i].connected_mc = c0;
        }
      }
    }
  }
  // one last check:
  if(main_input_module >= 0 && graph->module[main_input_module].connector[0].roi.wd == 0) return VK_INCOMPLETE;
} // end scope, done with modules

  // if no more action than generating the output roi was requested, exit now:
  if(run < s_graph_run_create_nodes<<1) return VK_SUCCESS;

{ // node scope
  int cnt = 0;
  dt_node_t *const arr = graph->node;
  const int arr_cnt = graph->num_nodes;
  uint32_t nodeid[2000];
  assert(sizeof(nodeid)/sizeof(nodeid[0]) >= graph->num_nodes);
#define TRAVERSE_POST \
  nodeid[cnt++] = curr;
#include "graph-traverse.inc"

  if(cnt == 0)
  {
    dt_log(s_log_pipe|s_log_err, "no nodes created!");
    return VK_INCOMPLETE;
  }

  // ==============================================
  // 1st pass alloc and free
  // ==============================================
  if(run & s_graph_run_alloc)
  {
    QVKR(dt_raytrace_graph_init(graph, nodeid, cnt)); // init ray tracing on graph, after output roi and nodes have been inited.
    // nuke reference counters:
    for(int n=0;n<graph->num_nodes;n++)
      for(int c=0;c<graph->node[n].num_connectors;c++)
        if(dt_connector_output(graph->node[n].connector+c))
          graph->node[n].connector[c].connected_mi = 0;
    // perform reference counting on the final connected node graph.
    // this is needed for memory allocation later:
    for(int i=0;i<cnt;i++)
      count_references(graph, graph->node+nodeid[i]);
    // free pipeline resources if previously allocated anything:
    dt_vkalloc_nuke(&graph->heap);
    dt_vkalloc_nuke(&graph->heap_ssbo);
    dt_vkalloc_nuke(&graph->heap_staging);
    graph->dset_cnt_image_read = 0;
    graph->dset_cnt_image_write = 0;
    graph->dset_cnt_buffer = 0;
    graph->dset_cnt_uniform = DT_GRAPH_MAX_FRAMES; // we have one global uniform for params, per frame
    graph->memory_type_bits = ~0u;
    graph->memory_type_bits_ssbo = ~0u;
    graph->memory_type_bits_staging = ~0u;
    for(int i=0;i<cnt;i++)
    {
      QVKR(alloc_outputs(graph, graph->node+nodeid[i]));
      QVKR(free_inputs  (graph, graph->node+nodeid[i]));
    }
  }

  if(graph->heap.vmsize > graph->vkmem_size)
  {
    run |= s_graph_run_upload_source; // new mem means new source
    if(graph->vkmem)
    {
      QVKLR(&qvk.queue_mutex, vkDeviceWaitIdle(qvk.device));
      vkFreeMemory(qvk.device, graph->vkmem, 0);
      graph->vkmem = 0;
    }
    // image data to pass between nodes
    VkMemoryAllocateInfo mem_alloc_info = {
      .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize  = graph->heap.vmsize,
      .memoryTypeIndex = qvk_get_memory_type(graph->memory_type_bits,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    QVKR(vkAllocateMemory(qvk.device, &mem_alloc_info, 0, &graph->vkmem));
    graph->vkmem_size = graph->heap.vmsize;
  }

  if(graph->heap_ssbo.vmsize > graph->vkmem_ssbo_size)
  {
    run |= s_graph_run_upload_source; // new mem means new source
    if(graph->vkmem_ssbo)
    {
      QVKLR(&qvk.queue_mutex, vkDeviceWaitIdle(qvk.device));
      vkFreeMemory(qvk.device, graph->vkmem_ssbo, 0);
      graph->vkmem_ssbo = 0;
    }
    // image data to pass between nodes
    VkMemoryAllocateInfo mem_alloc_info = {
      .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize  = graph->heap_ssbo.vmsize,
      .memoryTypeIndex = qvk_get_memory_type(graph->memory_type_bits_ssbo,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    QVKR(vkAllocateMemory(qvk.device, &mem_alloc_info, 0, &graph->vkmem_ssbo));
    graph->vkmem_ssbo_size = graph->heap_ssbo.vmsize;
  }

  if(graph->heap_staging.vmsize > graph->vkmem_staging_size)
  {
    run |= s_graph_run_upload_source; // new mem means new source
    if(graph->vkmem_staging)
    {
      QVKLR(&qvk.queue_mutex, vkDeviceWaitIdle(qvk.device));
      vkFreeMemory(qvk.device, graph->vkmem_staging, 0);
      graph->vkmem_staging = 0;
    }
    // staging memory to copy to and from device
    VkMemoryAllocateInfo mem_alloc_info_staging = {
      .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize  = graph->heap_staging.vmsize,
      .memoryTypeIndex = qvk_get_memory_type(graph->memory_type_bits_staging,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
    };
    QVKR(vkAllocateMemory(qvk.device, &mem_alloc_info_staging, 0, &graph->vkmem_staging));
    graph->vkmem_staging_size = graph->heap_staging.vmsize;
  }

  if(graph->vkmem_uniform_size < DT_GRAPH_MAX_FRAMES * graph->uniform_size)
  {
    if(graph->vkmem_uniform)
    {
      QVKLR(&qvk.queue_mutex, vkDeviceWaitIdle(qvk.device));
      vkFreeMemory(qvk.device, graph->vkmem_uniform, 0);
      graph->vkmem_uniform = 0;
    }
    // uniform data to pass parameters
    VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size  = DT_GRAPH_MAX_FRAMES * graph->uniform_size,
      .usage = // VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    vkDestroyBuffer(qvk.device, graph->uniform_buffer, 0);
    QVKR(vkCreateBuffer(qvk.device, &buffer_info, 0, &graph->uniform_buffer));
#ifdef DEBUG_MARKERS
#ifdef QVK_ENABLE_VALIDATION
    char name[100];
    const char *dedup;
    snprintf(name, sizeof(name), "uniform buffer");
    dt_stringpool_get(&graph->debug_markers, name, strlen(name), 0, &dedup);
    VkDebugMarkerObjectNameInfoEXT name_info = {
      .sType       = VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT,
      .object      = (uint64_t)graph->uniform_buffer,
      .objectType  = VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT,
      .pObjectName = dedup,
    };
    qvkDebugMarkerSetObjectNameEXT(qvk.device, &name_info);
#endif
#endif
    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(qvk.device, graph->uniform_buffer, &mem_req);
    VkMemoryAllocateInfo mem_alloc_info_uniform = {
      .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize  = mem_req.size,
      .memoryTypeIndex = qvk_get_memory_type(mem_req.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
    };
    QVKR(vkAllocateMemory(qvk.device, &mem_alloc_info_uniform, 0, &graph->vkmem_uniform));
    graph->vkmem_uniform_size = 2 * graph->uniform_size;
    vkBindBufferMemory(qvk.device, graph->uniform_buffer, graph->vkmem_uniform, 0);
  }

  if(run & s_graph_run_alloc)
  {
    if(graph->dset_pool &&
      graph->dset_cnt_image_read_alloc >= graph->dset_cnt_image_read &&
      graph->dset_cnt_image_write_alloc >= graph->dset_cnt_image_write &&
      graph->dset_cnt_buffer_alloc >= graph->dset_cnt_buffer &&
      graph->dset_cnt_uniform_alloc >= graph->dset_cnt_uniform)
    {
      QVKR(vkResetDescriptorPool(qvk.device, graph->dset_pool, 0));
    }
    else
    {
      // create descriptor pool (keep at least one for each type)
      VkDescriptorPoolSize pool_sizes[] = {{
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1+DT_GRAPH_MAX_FRAMES*graph->dset_cnt_image_read,
      }, {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1+DT_GRAPH_MAX_FRAMES*graph->dset_cnt_image_write,
      }, {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1+DT_GRAPH_MAX_FRAMES*graph->dset_cnt_buffer,
      }, {
        .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1+DT_GRAPH_MAX_FRAMES*2*graph->num_nodes,
      }, {
        .type            = qvk.raytracing_supported ?
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR :
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // dummy for validation layer
        .descriptorCount = DT_GRAPH_MAX_FRAMES,
      }};

      VkDescriptorPoolCreateInfo pool_info = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = LENGTH(pool_sizes),
        .pPoolSizes    = pool_sizes,
        .maxSets       = DT_GRAPH_MAX_FRAMES*(
            graph->dset_cnt_image_read + graph->dset_cnt_image_write
          + graph->num_nodes
          + graph->dset_cnt_buffer     + graph->dset_cnt_uniform),
      };
      vkDestroyDescriptorPool(qvk.device, graph->dset_pool, VK_NULL_HANDLE);
      graph->dset_pool = 0;
      QVKR(vkCreateDescriptorPool(qvk.device, &pool_info, 0, &graph->dset_pool));
      graph->dset_cnt_image_read_alloc = graph->dset_cnt_image_read;
      graph->dset_cnt_image_write_alloc = graph->dset_cnt_image_write;
      graph->dset_cnt_buffer_alloc = graph->dset_cnt_buffer;
      graph->dset_cnt_uniform_alloc = graph->dset_cnt_uniform;
    }
    // allocate memory and create descriptor sets for ray tracing
    QVKR(dt_raytrace_graph_alloc(graph));
  }

  // begin command buffer
  VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    // VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT would allow simultaneous execution while still pending.
    // not sure about our images, i suppose they will need sync/double buffering in this case
  };

  // ==============================================
  // 2nd pass finish alloc and record commmand buf
  // ==============================================
  if(run & s_graph_run_alloc)
    for(int i=0;i<cnt;i++)
      QVKR(alloc_outputs2(graph, graph->node+nodeid[i]));

  if(run & s_graph_run_alloc)
    for(int i=0;i<cnt;i++)
      QVKR(alloc_outputs3(graph, graph->node+nodeid[i]));

  // find dynamically allocated connector in node that has array requests set:
  int dynamic_array = 0; // will remain 0 if there are dynamic arrays that did not request changes, triggers upload later
  for(int i=0;i<cnt;i++)
  {
    dt_node_t *node = graph->node + nodeid[i];
    for(int j=0;j<node->num_connectors;j++)
    {
      dt_connector_t *c = node->connector+j;
      if(c->array_resync)
      { // last frame has pending sync work to do
        for(int a=0;a<MAX(1,c->array_length);a++)
        { // keep array images the same in general, but delete images that wandered off the command buffer bindings:
          dt_connector_image_t *oldimg = dt_graph_connector_image(graph, node-graph->node, j, a, graph->frame+1);
          dt_connector_image_t *newimg = dt_graph_connector_image(graph, node-graph->node, j, a, graph->frame);
          if(!oldimg || !newimg) continue; // will insert dummy later
          if(oldimg->image == newimg->image) continue; // is the same already
          if(newimg->buffer)     vkDestroyBuffer(qvk.device,    newimg->buffer, VK_NULL_HANDLE);
          if(newimg->image)      vkDestroyImage(qvk.device,     newimg->image, VK_NULL_HANDLE);
          if(newimg->image_view) vkDestroyImageView(qvk.device, newimg->image_view, VK_NULL_HANDLE);
          if(newimg->mem)        dt_vkfree(c->array_alloc, newimg->mem);
          *newimg = *oldimg; // point to same memory
          dynamic_array = 1; // need to re-create descriptor sets
        }
        c->array_resync = 0; // done with this request
      }
      if(dt_connector_output(c) && (c->flags & s_conn_dynamic_array) && c->array_req)
      for(int k=0;k<MAX(1,c->array_length);k++)
      {
        if(k == 0 && !c->array_req[0])
        { // make sure the fallback slot 0 is always inited with at least rubbish:
          dt_connector_image_t *img = dt_graph_connector_image(graph, nodeid[i], j, k, f);
          assert(img);
          if(!img->image_view)
          {
            *img = (dt_connector_image_t){0}; // make sure nothing is freed here
            QVKR(allocate_image_array_element(graph, node, c, c->array_alloc, c->array_mem->offset, f, k));
            QVKR(bind_buffers_to_memory(graph, node, c, f, k));
          }
        }
        if(c->array_req[k])
        { // free texture. we depend on f-1 still holding our data, it will be freed during resync.
          dynamic_array = c->array_resync = 1; // remember to bring other frame in sync next time to pick up our changes
          dt_connector_image_t *img = dt_graph_connector_image(graph, nodeid[i], j, k, f);
          assert(img);
          *img = (dt_connector_image_t){0}; // make sure nothing is freed here
          QVKR(allocate_image_array_element(graph, node, c, c->array_alloc, c->array_mem->offset, f, k));
          QVKR(bind_buffers_to_memory(graph, node, c, f, k));
        }
        // upload is handled below
      }
    } // end for all connectors
  }
  // write the dynamic array descriptor sets separately (this is skipped in alloc_outputs3 above).
  // this writes both the output and connected input descriptors.
  if(dynamic_array)
    for(int i=0;i<cnt;i++) for(int j=0;j<graph->node[nodeid[i]].num_connectors;j++)
      write_descriptor_sets(graph, graph->node+nodeid[i], graph->node[nodeid[i]].connector + j, 1);

  // upload all source data to staging memory
  threads_mutex_t *mutex = 0;// graph->io_mutex; // no speed impact, maybe not needed
  if(mutex) threads_mutex_lock(mutex);
  if( dynamic_array ||
     (module_flags & s_module_request_read_source) ||
     (run & s_graph_run_upload_source))
  {
    double upload_beg = dt_time();
    uint8_t *mapped = 0;
    QVKR(vkMapMemory(qvk.device, graph->vkmem_staging, 0, VK_WHOLE_SIZE, 0, (void**)&mapped));
    for(int n=0;n<graph->num_nodes;n++)
    { // for all source nodes:
      dt_node_t *node = graph->node + n;
      if(dt_node_source(node))
      {
        if(node->module->so->read_source)
        {
          int run_node = (node->flags & s_module_request_read_source) ||
                         (run & s_graph_run_upload_source);
          const int c = 0;
          if(run_node || (dynamic_array && (node->connector[c].flags & s_conn_dynamic_array)))
          {
            for(int a=0;a<MAX(1,node->connector[c].array_length);a++)
            {
              if(node->connector[c].array_req)
              {
                if(!(node->connector[c].array_req[a])) continue;
                node->connector[c].array_req[a] = 0; // clear image load request
              }
              dt_read_source_params_t p = { .node = node, .c = c, .a = a };
              node->module->so->read_source(node->module,
                  mapped + node->connector[c].offset_staging, &p);
              if(node->connector[c].array_length > 1)
              {
                if(!dt_graph_connector_image(graph, node-graph->node, c, a, graph->frame)->image)
                  continue;
                vkUnmapMemory(qvk.device, graph->vkmem_staging);
                const uint32_t wd = MAX(1, node->connector[c].array_dim ? node->connector[c].array_dim[2*a+0] : node->connector[c].roi.wd);
                const uint32_t ht = MAX(1, node->connector[c].array_dim ? node->connector[c].array_dim[2*a+1] : node->connector[c].roi.ht);
                VkBufferImageCopy regions[] = {{
                  .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .imageSubresource.layerCount = 1,
                  .imageExtent = { wd, ht, 1 },
                },{
                  .imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
                  .imageSubresource.layerCount = 1,
                  .imageExtent = { wd, ht, 1 },
                },{
                  .bufferOffset = dt_graph_connector_image(graph, node-graph->node, c, a, graph->frame)->plane1_offset,
                  .imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
                  .imageSubresource.layerCount = 1,
                  .imageExtent = { wd / 2, ht / 2, 1 },
                }};
                const int yuv = node->connector[c].format == dt_token("yuv");
                VkCommandBuffer cmd_buf = graph->command_buffer[f];
                QVKR(vkBeginCommandBuffer(cmd_buf, &begin_info));
                IMG_LAYOUT(
                    dt_graph_connector_image(graph, node-graph->node, c, a, graph->frame),
                    UNDEFINED,
                    TRANSFER_DST_OPTIMAL);
                vkCmdCopyBufferToImage(
                    cmd_buf,
                    node->connector[c].staging,
                    dt_graph_connector_image(graph, node-graph->node, c, a, graph->frame)->image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    yuv ? 2 : 1, yuv ? regions+1 : regions);
                IMG_LAYOUT(
                    dt_graph_connector_image(graph, node-graph->node, c, a, graph->frame),
                    TRANSFER_DST_OPTIMAL,
                    SHADER_READ_ONLY_OPTIMAL);
                QVKR(vkEndCommandBuffer(cmd_buf));
                VkSubmitInfo submit = {
                  .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                  .commandBufferCount = 1,
                  .pCommandBuffers    = &cmd_buf,
                };
                vkResetFences(qvk.device, 1, &graph->command_fence[f]);
                QVKLR(graph->queue_mutex, vkQueueSubmit(graph->queue, 1, &submit, graph->command_fence[f]));
                QVKR(vkWaitForFences(qvk.device, 1, &graph->command_fence[f], VK_TRUE, ((uint64_t)1)<<40)); // wait inline on our lock because we share the staging buf
                QVKR(vkMapMemory(qvk.device, graph->vkmem_staging, 0, VK_WHOLE_SIZE, 0, (void**)&mapped));
              }
            }
          }
        }
        else
          dt_log(s_log_err|s_log_pipe, "source node '%"PRItkn"' has no read_source() callback!",
              dt_token_str(node->name));
      }
    }
    vkUnmapMemory(qvk.device, graph->vkmem_staging);
    double upload_end = dt_time();
    dt_log(s_log_perf, "upload source total:\t%8.3f ms", 1000.0*(upload_end-upload_beg));
  }
  if(mutex) threads_mutex_unlock(mutex);

  if(run & s_graph_run_record_cmd_buf)
  {
    QVKR(vkBeginCommandBuffer(graph->command_buffer[f], &begin_info));
    graph->query[f].cnt = 0;
    vkCmdResetQueryPool(graph->command_buffer[f], graph->query[f].pool, 0, graph->query[f].max);
    double rt_beg = dt_time();
    int run_all = run & s_graph_run_upload_source;
    int run_mod = module_flags & s_module_request_read_geo;
    if(run_all || run_mod) QVKR(dt_raytrace_record_command_buffer_accel_build(graph));
    double rt_end = dt_time();
    dt_log(s_log_perf, "create raytrace accel:\t%8.3f ms", 1000.0*(rt_end-rt_beg));
    rt_beg = rt_end;
    for(int i=0;i<cnt;i++)
    {
      VkResult res = record_command_buffer(graph, graph->node+nodeid[i], run_all ||
          (graph->node[nodeid[i]].module->flags & s_module_request_read_source));
      if(res != VK_SUCCESS)
      { // need to clean up command buffer before we quit
        QVKR(vkEndCommandBuffer(graph->command_buffer[f]));
        return res;
      }
    }
    rt_end = dt_time();
    dt_log(s_log_perf, "record command buffer:\t%8.3f ms", 1000.0*(rt_end-rt_beg));
    QVKR(vkEndCommandBuffer(graph->command_buffer[f]));
  }
} // end scope, done with nodes

  if(run & s_graph_run_alloc)
  {
    dt_log(s_log_mem, "images : peak rss %g MB vmsize %g MB",
        graph->heap.peak_rss/(1024.0*1024.0),
        graph->heap.vmsize  /(1024.0*1024.0));
    dt_log(s_log_mem, "buffers: peak rss %g MB vmsize %g MB",
        graph->heap_ssbo.peak_rss/(1024.0*1024.0),
        graph->heap_ssbo.vmsize  /(1024.0*1024.0));
    dt_log(s_log_mem, "staging: peak rss %g MB vmsize %g MB",
        graph->heap_staging.peak_rss/(1024.0*1024.0),
        graph->heap_staging.vmsize  /(1024.0*1024.0));
  }

  // now upload uniform data before submitting command buffer
{ // module traversal
  dt_module_t *const arr = graph->module;
  const int arr_cnt = graph->num_modules;
  uint8_t *uniform_mem = 0;
  QVKR(vkMapMemory(qvk.device, graph->vkmem_uniform, ((uint64_t)f) * graph->uniform_size,
        graph->uniform_size, 0, (void**)&uniform_mem));
  ((uint32_t *)uniform_mem)[0] = graph->frame;
  ((uint32_t *)uniform_mem)[1] = graph->frame_cnt;
#define TRAVERSE_POST \
  dt_module_t *mod = arr+curr;\
  if(mod->so->commit_params)\
    mod->so->commit_params(graph, mod);\
  if(mod->committed_param_size)\
    memcpy(uniform_mem + mod->uniform_offset, mod->committed_param, mod->committed_param_size);\
  else if(mod->param_size)\
    memcpy(uniform_mem + mod->uniform_offset, mod->param, mod->param_size);
#include "graph-traverse.inc"
  vkUnmapMemory(qvk.device, graph->vkmem_uniform);
}

  double clock_end = dt_time();
  dt_log(s_log_perf, "record cmd buffer:\t%8.3f ms", 1000.0*(clock_end - clock_beg));

  VkSubmitInfo submit = {
    .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers    = &graph->command_buffer[f],
  };

  if(run & s_graph_run_record_cmd_buf)
  {
    vkResetFences(qvk.device, 1, &graph->command_fence[f]);
    QVKLR(graph->queue_mutex, vkQueueSubmit(graph->queue, 1, &submit, graph->command_fence[f]));
    if(run & s_graph_run_wait_done) // timeout in nanoseconds, 30 is about 1s
      QVKR(vkWaitForFences(qvk.device, 1, &graph->command_fence[f], VK_TRUE, ((uint64_t)1)<<40)); // wait for our command buffer
    else
      QVKR(vkWaitForFences(qvk.device, 1, &graph->command_fence[fp], VK_TRUE, ((uint64_t)1)<<40)); // wait for previous command buffer
  }
  
  // XXX FIXME: this is a race condition for multi-frames. we'll need to wait until download is complete before starting the other command buffer!
  // XXX FIXME: maybe lock the queue mutex around the whole block?
  // XXX FIXME: may need an entirely different logic block for single frame?
  if((module_flags & s_module_request_write_sink) ||
     (run & s_graph_run_download_sink))
  {
    for(int n=0;n<graph->num_nodes;n++)
    { // for all sink nodes:
      dt_node_t *node = graph->node + n;
      if(dt_node_sink(node))
      {
        if(node->module->so->write_sink &&
          ((node->module->flags & s_module_request_write_sink) ||
           (run & s_graph_run_download_sink)))
        {
          uint8_t *mapped = 0;
          QVKR(vkMapMemory(qvk.device, graph->vkmem_staging, 0, VK_WHOLE_SIZE,
                0, (void**)&mapped));
          node->module->so->write_sink(node->module,
              mapped + node->connector[0].offset_staging);
          vkUnmapMemory(qvk.device, graph->vkmem_staging);
        }
      }
    }
  }

  if(dt_log_global.mask & s_log_perf)
  {
    int q = fp;
    if(run & s_graph_run_wait_done) q = f; // if we're synchronous, use the one we just waited for
    if(graph->query[q].cnt) // could store the results just once, but for separation of concerns they are part of the struct:
      QVKR(vkGetQueryPoolResults(qvk.device, graph->query[q].pool,
          0, graph->query[q].cnt,
          sizeof(graph->query[q].pool_results[0]) * graph->query[q].max,
          graph->query[q].pool_results,
          sizeof(graph->query[q].pool_results[0]),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));

    uint64_t accum_time = 0;
    dt_token_t last_name = 0;
    for(int i=0;i<graph->query[q].cnt;i+=2)
    {
      if(i < graph->query[q].cnt-2 && (
         graph->query[q].name[i] == last_name || !last_name ||
         graph->query[q].name[i] == dt_token("shared") || last_name == dt_token("shared")))
        accum_time += graph->query[q].pool_results[i+1] - graph->query[q].pool_results[i];
      else
      {
        if(i && accum_time > graph->query[q].pool_results[i-1] - graph->query[q].pool_results[i-2])
          dt_log(s_log_perf, "sum %"PRItkn":\t%8.3f ms",
              dt_token_str(last_name),
              accum_time * 1e-6 * qvk.ticks_to_nanoseconds);
        if(i < graph->query[q].cnt-2)
          accum_time = graph->query[q].pool_results[i+1] - graph->query[q].pool_results[i];
      }
      last_name = graph->query[q].name[i];
      // i think this is the most horrible line of printf i've ever written:
      dt_log(s_log_perf, "%-*.*s %-*.*s:\t%8.3f ms",
          8, 8, dt_token_str(graph->query[q].name  [i]),
          8, 8, dt_token_str(graph->query[q].kernel[i]),
          (graph->query[q].pool_results[i+1]-
          graph->query[q].pool_results[i])* 1e-6 * qvk.ticks_to_nanoseconds);
    }
    if(graph->query[q].cnt)
    {
      graph->query[q].last_frame_duration = (graph->query[q].pool_results[graph->query[q].cnt-1]-graph->query[q].pool_results[0])*1e-6 * qvk.ticks_to_nanoseconds;
      dt_log(s_log_perf, "total time:\t%8.3f ms", graph->query[q].last_frame_duration);
    }
  }
  // reset run flags and gui error message
  graph->runflags = 0;
  graph->gui_msg = 0;
  return VK_SUCCESS;
}

dt_node_t *
dt_graph_get_display(
    dt_graph_t *g,
    dt_token_t  which)
{
  // TODO: cache somewhere on graph?
  for(int n=0;n<g->num_nodes;n++)
  {
    if((g->node[n].module->name == dt_token("display") ||
        g->node[n].module->name == dt_token("thumb")) &&
        g->node[n].module->inst == which)
      return g->node+n;
  }
  return 0;
}

dt_connector_image_t*
dt_graph_connector_image(
    dt_graph_t *graph,
    int         nid,    // node id
    int         cid,    // connector id
    int         array,  // array index
    int         frame)  // frame number
{
  if(graph->node[nid].conn_image[cid] == -1)
  {
    dt_log(s_log_err, "requesting disconnected image buffer!");
    return 0;
  }
  int nid2 = nid, cid2 = cid;
  if(dt_connector_input(graph->node[nid].connector+cid))
  {
    cid2 = graph->node[nid].connector[cid].connected_mc;
    nid2 = graph->node[nid].connector[cid].connected_mi;
  }
  frame %= MAX(1, graph->node[nid2].connector[cid2].frames);
  return graph->conn_image_pool +
    graph->node[nid].conn_image[cid] + MAX(1,graph->node[nid].connector[cid].array_length) * frame + array;
}

void dt_graph_reset(dt_graph_t *g)
{
#ifdef DEBUG_MARKERS
  dt_stringpool_reset(&g->debug_markers);
#endif
  dt_raytrace_graph_reset(g);
  g->gui_attached = 0;
  g->gui_msg = 0;
  g->active_module = 0;
  g->lod_scale = 0;
  g->runflags = 0;
  g->frame = 0;
  g->output_wd = 0;
  g->output_ht = 0;
  g->thumbnail_image = 0;
  g->query[0].cnt = g->query[1].cnt = 0;
  g->params_end = 0;
  for(int i=0;i<g->num_modules;i++)
    if(g->module[i].name && g->module[i].so->cleanup)
      g->module[i].so->cleanup(g->module+i);
  for(int i=0;i<g->conn_image_end;i++)
  {
    if(g->conn_image_pool[i].image)      vkDestroyImage(qvk.device,     g->conn_image_pool[i].image, VK_NULL_HANDLE);
    if(g->conn_image_pool[i].image_view) vkDestroyImageView(qvk.device, g->conn_image_pool[i].image_view, VK_NULL_HANDLE);
    g->conn_image_pool[i].image = 0;
    g->conn_image_pool[i].image_view = 0;
  }
  for(int i=0;i<g->num_nodes;i++)
  {
    for(int j=0;j<g->node[i].num_connectors;j++)
    {
      dt_connector_t *c = g->node[i].connector+j;
      if(c->staging) vkDestroyBuffer(qvk.device, c->staging, VK_NULL_HANDLE);
      c->staging = 0;
      if(c->array_alloc)
      { // free any potential residuals of dynamic allocation
        dt_vkalloc_cleanup(c->array_alloc);
        free(c->array_alloc);
        c->array_alloc = 0;
        c->array_alloc_size = 0;
        c->array_mem = 0;
      }
    }
    vkDestroyPipelineLayout     (qvk.device, g->node[i].pipeline_layout,  0);
    vkDestroyPipeline           (qvk.device, g->node[i].pipeline,         0);
    vkDestroyDescriptorSetLayout(qvk.device, g->node[i].dset_layout,      0);
    vkDestroyFramebuffer        (qvk.device, g->node[i].draw_framebuffer, 0);
    vkDestroyRenderPass         (qvk.device, g->node[i].draw_render_pass, 0);
    g->node[i].pipeline_layout = 0;
    g->node[i].pipeline = 0;
    g->node[i].dset_layout = 0;
    g->node[i].draw_framebuffer = 0;
    g->node[i].draw_render_pass = 0;
  }
  g->conn_image_end = 0;
  g->num_nodes = 0;
  g->num_modules = 0;
}

void
dt_graph_apply_keyframes(
    dt_graph_t *g)
{
  for(int m=0;m<g->num_modules;m++)
  {
    if(g->module[m].name == 0) continue; // skip deleted modules
    dt_keyframe_t *kf = g->module[m].keyframe;
    if(g->module[m].keyframe_cnt) for(int p=0;p<g->module[m].so->num_params;p++)
    {
      int ki = -1, kiM = -1; // find ki.f <= f < kiM.f
      for(int i=0;i<g->module[m].keyframe_cnt;i++)
      { // search for max frame smaller than current frame with same param
        if(kf[i].param == g->module[m].so->param[p]->name)
        {
          if(ki == -1) ki = i; // always accept if we don't have any so far
          else if(kf[ki].frame >  g->frame && kf[i].frame < kf[ki].frame) // if we only have an emergency frame so far use the <
            ki = i;
          else if(kf[ki].frame <= g->frame && kf[i].frame <= g->frame && kf[i].frame > kf[ki].frame) // if we have one that is actually lower the new one needs to be <= too
            ki = i;
        }
      }
      if(ki == -1) continue; // no keyframe for this parameter

      for(int i=0;i<g->module[m].keyframe_cnt;i++)
      { // now search for later frame to interpolate
        if(kf[i].param == g->module[m].so->param[p]->name)
          if(kf[i].frame > g->frame && (kiM == -1 || kf[kiM].frame > kf[i].frame)) kiM = i;
      }
      if(kiM == ki) kiM = -1; // don't interpolate same frame
      int parid = dt_module_get_param(g->module[m].so, g->module[m].so->param[p]->name);
      const dt_ui_param_t *p = g->module[m].so->param[parid];
      uint8_t *pdat = g->module[m].param + p->offset;
      uint8_t *fdat = kf[ki].data;
      size_t els = dt_ui_param_size(p->type, 1);
      const float t = (g->frame - kf[ki].frame)/(float)(kf[kiM].frame - kf[ki].frame);
      if(kiM >= 0 && p->type == dt_token("float")) // TODO: don't search for kiM above in the other cases?
      { // interpolate generic floating point parameters
        float *dst = (float *)pdat, *src0 = (float *)fdat, *src1 = (float *)kf[kiM].data;
        for(int i=kf[ki].beg;i<kf[ki].end;i++)
          dst[i] = t * src1[i-kf[ki].beg] + (1.0f-t) * src0[i-kf[ki].beg];
      }
      else if(kiM >= 0 && p->name == dt_token("draw"))
      { // interpolate drawn list of vertices
        uint32_t *dst = (uint32_t *)pdat, *src0 = (uint32_t *)fdat, *src1 = (uint32_t *)kf[kiM].data;
        int vcnt = MIN(src0[0], src1[0]); // can only interpolate what we have on both ends
        dst[0] = vcnt;
        dt_draw_vert_t *vd = (dt_draw_vert_t *)(dst +1);
        dt_draw_vert_t *v0 = (dt_draw_vert_t *)(src0+1);
        dt_draw_vert_t *v1 = (dt_draw_vert_t *)(src1+1);
        for(int i=0;i<vcnt;i++)
        {
          if(dt_draw_eq(v0[i], dt_draw_endmarker()) ||
             dt_draw_eq(v1[i], dt_draw_endmarker()))
          { // use symmetric end markers
            vd[i] = dt_draw_endmarker();
          }
          else
          { // interpolate properties
            // XXX FIXME: beg and end are currently not supported. once we have a "draw" type they should mean
            // XXX FIXME: vertex indices, i.e. this here would need to be v0[i - kf[ki].beg] and the loop above
            // XXX FIXME: would need to be MIN(vcnt, kf[ki].end-kf[ki].beg)
            vd[i] = dt_draw_mix(v0[i], v1[i], t);
          }
        }
        g->module[m].flags = s_module_request_read_source; // make sure the draw list is updated
      }
      else
      { // apply directly
        memcpy(pdat, fdat + els*kf[ki].beg, els*(kf[ki].end-kf[ki].beg));
      }
    }
  }
}
