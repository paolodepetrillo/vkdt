#include "modules/api.h"
#include "callback.h"

void write_sink(
    dt_module_t *module,
    void *buf)
{
  dt_ocback_data_t *data = (dt_ocback_data_t *)module->data;
  if(data != NULL)
    if (data->callback != NULL)
      data->callback(data->param, module->inst, module->connector[0].roi.wd, module->connector[0].roi.ht, buf);
}
