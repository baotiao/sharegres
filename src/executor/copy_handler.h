#ifndef SHAREGRES_COPY_HANDLER_H
#define SHAREGRES_COPY_HANDLER_H

#include "../session/session.h"

/*
 * Handle COPY IN data from the client: forward CopyData/CopyDone/CopyFail
 * messages to the backend that initiated the COPY.
 *
 * Returns:
 *   0  - message forwarded, stay in COPY state
 *   1  - COPY finished (CopyDone/CopyFail received)
 *  -1  - error
 */
int copy_handle_client_message(ClientSession *session, uint8_t msg_type,
                               const uint8_t *payload, size_t payload_len);

/*
 * Called when the backend sends a CopyInResponse or CopyOutResponse.
 * Forwards it to the client and switches session to COPY mode.
 */
void copy_forward_response(ClientSession *session, const uint8_t *raw_msg,
                           size_t msg_len);

#endif /* SHAREGRES_COPY_HANDLER_H */
