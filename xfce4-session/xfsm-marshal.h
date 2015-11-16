
#ifndef __xfsm_marshal_MARSHAL_H__
#define __xfsm_marshal_MARSHAL_H__

#include	<glib-object.h>

G_BEGIN_DECLS

/* VOID:UINT,UINT (xfsm-marshal.list:1) */
G_GNUC_INTERNAL void xfsm_marshal_VOID__UINT_UINT (GClosure     *closure,
                                                   GValue       *return_value,
                                                   guint         n_param_values,
                                                   const GValue *param_values,
                                                   gpointer      invocation_hint,
                                                   gpointer      marshal_data);

/* VOID:STRING,BOXED (xfsm-marshal.list:2) */
G_GNUC_INTERNAL void xfsm_marshal_VOID__STRING_BOXED (GClosure     *closure,
                                                      GValue       *return_value,
                                                      guint         n_param_values,
                                                      const GValue *param_values,
                                                      gpointer      invocation_hint,
                                                      gpointer      marshal_data);

G_END_DECLS

#endif /* __xfsm_marshal_MARSHAL_H__ */

