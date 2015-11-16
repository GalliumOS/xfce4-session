
#ifndef __xfce4_session_marshal_MARSHAL_H__
#define __xfce4_session_marshal_MARSHAL_H__

#include	<glib-object.h>

G_BEGIN_DECLS

/* VOID:UINT,UINT (xfce4-session-marshal.list:1) */
extern void xfce4_session_marshal_VOID__UINT_UINT (GClosure     *closure,
                                                   GValue       *return_value,
                                                   guint         n_param_values,
                                                   const GValue *param_values,
                                                   gpointer      invocation_hint,
                                                   gpointer      marshal_data);

/* VOID:STRING,BOXED (xfce4-session-marshal.list:2) */
extern void xfce4_session_marshal_VOID__STRING_BOXED (GClosure     *closure,
                                                      GValue       *return_value,
                                                      guint         n_param_values,
                                                      const GValue *param_values,
                                                      gpointer      invocation_hint,
                                                      gpointer      marshal_data);

G_END_DECLS

#endif /* __xfce4_session_marshal_MARSHAL_H__ */

