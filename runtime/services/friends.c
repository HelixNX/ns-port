/* friend:u: friends, offline. No friends, no play history; presence updates are accepted and dropped.
 * Command ids are from the SDK 4.4 proxies (IServiceCreator, IFriendService, INotificationService). */
#include "../ipc.h"
#include <string.h>

static KEvent *completion_event, *notification_event;

static void fs_completion_event(SvcObj *o, IpcReq *r) { out_event(r, completion_event); }
static void fs_zero_count(SvcObj *o, IpcReq *r) { out_u32(r, 0); }
static void fs_zero_buffer(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_out(r, 0);
    if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);
}

static const Iface iface_friend = {"IFriendService", (const IpcCmd[]){
    {0, fs_completion_event, "GetCompletionEvent"},
    {1, ipc_noop, "Cancel"},
    {10100, fs_zero_count, "GetFriendListIds"},
    {10101, fs_zero_count, "GetFriendList"},
    {10102, ipc_noop, "UpdateFriendInfo"},
    {10400, fs_zero_count, "GetBlockedUserListIds"},
    {10500, ipc_noop, "GetProfileList"},
    {10600, ipc_noop, "DeclareOpenOnlinePlaySession"},
    {10601, ipc_noop, "DeclareCloseOnlinePlaySession"},
    {10610, ipc_noop, "UpdateUserPresence"},
    {10700, fs_zero_buffer, "GetPlayHistoryRegistrationKey"},
    {10701, fs_zero_buffer, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId"},
    {10702, ipc_noop, "AddPlayHistory"},
    {20100, fs_zero_count, "GetFriendCount"},
    {20101, fs_zero_count, "GetNewlyFriendCount"},
    IPC_END}, NULL};

static void ns_event(SvcObj *o, IpcReq *r) { out_event(r, notification_event); }
static void ns_pop(SvcObj *o, IpcReq *r) { r->result = 121 | (6 << 9); }   /* friends: no notification (never signalled) */

static const Iface iface_notification = {"INotificationService", (const IpcCmd[]){
    {0, ns_event, "GetEvent"},
    {1, ipc_noop, "Clear"},
    {2, ns_pop, "Pop"},
    IPC_END}, NULL};

IPC_OPEN(fr_create_friend, &iface_friend)
IPC_OPEN(fr_create_notification, &iface_notification)

static const Iface iface_creator = {"friend", (const IpcCmd[]){
    {0, fr_create_friend, "CreateFriendService"},
    {1, fr_create_notification, "CreateNotificationService"},
    IPC_END}, NULL};

static SvcObj *open_friend(const char *name) { return svcobj_new(&iface_creator, NULL, name); }

void friends_register(void) {
    completion_event = event_create();
    notification_event = event_create();
    service_register("friend:u", open_friend);
    service_register("friend:v", open_friend);
    service_register("friend:a", open_friend);
}
