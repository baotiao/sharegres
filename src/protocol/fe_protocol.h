#ifndef SHAREGRES_FE_PROTOCOL_H
#define SHAREGRES_FE_PROTOCOL_H

#include "../session/session.h"

/*
 * Process data in the client's receive buffer.
 * Returns 0 on success, -1 on error (session should be closed).
 */
int fe_protocol_process(ClientSession *session);

/*
 * Send the initial handshake response to the client.
 */
void fe_send_startup_response(ClientSession *session);

/*
 * Send error response to client.
 */
void fe_send_error(ClientSession *session, const char *severity,
                   const char *code, const char *message);

/*
 * Forward result rows from PGresult to client.
 * Returns number of rows forwarded.
 * (Declared here but implemented in session.c)
 */
int session_forward_result(ClientSession *session, PGresult *res, bool send_header);

#endif /* SHAREGRES_FE_PROTOCOL_H */
