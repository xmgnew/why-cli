#ifndef WHY_IPC_H
#define WHY_IPC_H
#include "history.h"
#include <signal.h>

/* Linux uses a filesystem-free abstract endpoint; macOS pathname transport
 * currently exists for portable tests only. One same-UID client at a time.
 * Sockets and response buffers are bounded; history is borrowed only
 * synchronously while rendering a response. */
typedef struct WhyServer WhyServer;
WhyServer *why_server_open(void);
void why_server_close(WhyServer *server);
/* Services requests until the next collection deadline or stop signal. */
bool why_server_wait(WhyServer *server, const WhyHistory *history, long hz,
					 uint64_t deadline, const volatile sig_atomic_t *stopped);
int why_client_query(unsigned seconds);
#endif
