#include "modules/api.h"
#include <math.h>
#include <stdlib.h>

void
commit_params(
    dt_graph_t  *graph,
    dt_module_t *module)
{
  float *f = (float *)module->committed_param;
  float *g = (float*)module->param;
  f[0] = g[0];
  f[1] = g[1];
  f[2] = g[2];
  f[3] = 0.0f;// module->img_param.black[1]; // XXX TODO: set this to zero after removing black level!
  // TODO: how to do this? we can't overwrite img_param in denoise since we need it next time
  // TODO: and we can't access the (potentially multiple) modules connected to our output
  f[4] = module->img_param.noise_a;
  f[5] = module->img_param.noise_b;
}

int
init(dt_module_t *mod)
{
  mod->committed_param_size = sizeof(float)*6;
  return 0;
}

void
create_nodes(
    dt_graph_t  *graph,
    dt_module_t *module)
{
  const int wd = module->connector[0].roi.wd;
  const int ht = module->connector[0].roi.ht;
  const int dp = 1;

  // wire 4 scales of downsample + assembly node
  int id_down[4] = {0};

  for(int i=0;i<4;i++)
  {
    assert(graph->num_nodes < graph->max_nodes);
    id_down[i] = graph->num_nodes++;
    graph->node[id_down[i]] = (dt_node_t) {
      .name   = dt_token("dev"),
      .kernel = dt_token("down"),
      .module = module,
      .wd     = wd,
      .ht     = ht,
      .dp     = dp,
      .num_connectors = 2,
      .connector = {{
        .name   = dt_token("input"),
        .type   = dt_token("read"),
        .chan   = dt_token("rgba"),
        .format = dt_token("f16"),
        .roi    = module->connector[0].roi,
        .connected_mi = -1,
      },{
        .name   = dt_token("output"),
        .type   = dt_token("write"),
        .chan   = dt_token("rgba"),
        .format = dt_token("f16"),
        .roi    = module->connector[0].roi,
      }},
      .push_constant_size = sizeof(uint32_t),
      .push_constant = { i },
    };
  }
  // wire inputs:
  for(int i=1;i<4;i++)
    CONN(dt_node_connect(graph, id_down[i-1], 1, id_down[i], 0));

  // assemble
  assert(graph->num_nodes < graph->max_nodes);
  const uint32_t id_assemble = graph->num_nodes++;
  graph->node[id_assemble] = (dt_node_t) {
    .name   = dt_token("dev"),
    .kernel = dt_token("assemble"),
    .module = module,
    .wd     = wd,
    .ht     = ht,
    .dp     = dp,
    .num_connectors = 6,
    .connector = {{
      .name   = dt_token("s0"),
      .type   = dt_token("read"),
      .chan   = dt_token("rgba"),
      .format = dt_token("f16"),
      .roi    = module->connector[0].roi,
      .connected_mi = -1,
    },{
      .name   = dt_token("s1"),
      .type   = dt_token("read"),
      .chan   = dt_token("rgba"),
      .format = dt_token("f16"),
      .roi    = module->connector[0].roi,
      .connected_mi = -1,
    },{
      .name   = dt_token("s2"),
      .type   = dt_token("read"),
      .chan   = dt_token("rgba"),
      .format = dt_token("f16"),
      .roi    = module->connector[0].roi,
      .connected_mi = -1,
    },{
      .name   = dt_token("s3"),
      .type   = dt_token("read"),
      .chan   = dt_token("rgba"),
      .format = dt_token("f16"),
      .roi    = module->connector[0].roi,
      .connected_mi = -1,
    },{
      .name   = dt_token("s4"),
      .type   = dt_token("read"),
      .chan   = dt_token("rgba"),
      .format = dt_token("f16"),
      .roi    = module->connector[0].roi,
      .connected_mi = -1,
    },{
      .name   = dt_token("output"),
      .type   = dt_token("write"),
      .chan   = dt_token("rgba"),
      .format = dt_token("f16"),
      .roi    = module->connector[0].roi,
    }},
  };

  // wire downsampled to assembly stage:
  CONN(dt_node_connect(graph, id_down[0], 1, id_assemble, 1));
  CONN(dt_node_connect(graph, id_down[1], 1, id_assemble, 2));
  CONN(dt_node_connect(graph, id_down[2], 1, id_assemble, 3));
  CONN(dt_node_connect(graph, id_down[3], 1, id_assemble, 4));

  // wire module i/o connectors to nodes:
  dt_connector_copy(graph, module, 0, id_down[0], 0);
  dt_connector_copy(graph, module, 0, id_assemble, 0);
  dt_connector_copy(graph, module, 1, id_assemble, 5);
}

