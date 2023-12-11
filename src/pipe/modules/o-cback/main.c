#include "modules/api.h"
#include "callback.h"

void write_sink(
    dt_module_t *module,
    void *buf)
{
  dt_output_callback_t cb = (dt_output_callback_t)module->data;
  if(cb != NULL)
    cb(module->inst, module->connector[0].roi.wd, module->connector[0].roi.ht, buf);
}
