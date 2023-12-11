#include "modules/api.h"
#include "pipe/callback.h"

void write_sink(
    dt_module_t *module,
    void *buf)
{
  if(dt_output_callback == NULL)
    return;

  dt_output_callback(module->inst, module->connector[0].roi.wd, module->connector[0].roi.ht, buf);
}
