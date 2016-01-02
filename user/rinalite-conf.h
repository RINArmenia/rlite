#ifndef __RINALITE_CONF_H__
#define __RINALITE_CONF_H__

#include "rinalite-evloop.h"


#ifdef __cplusplus
extern "C" {
#endif

int rinalite_ipcp_config(struct rinalite_evloop *loop, uint16_t ipcp_id,
                         const char *param_name, const char *param_value);

#ifdef __cplusplus
}
#endif

#endif  /* __RINALITE_CONF_H__ */
