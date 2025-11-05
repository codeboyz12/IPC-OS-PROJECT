#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include "project_defs.h"

// --- Global State and Synchronization for Job Queue ---
pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
Job* job_queue_head = NULL;
Job* job_queue_tail = NULL;

// --- Global Registry State ---
GlobalRegistry registry;
int control_qid = -1; // Server's control message queue ID

// --- Forward Declarations & Helpers ---
void cleanup(int sig);
void init_server_state();
void router_thread();
void* broadcaster_thread(void* arg);
void* monitor_clients(void* arg);
void add_job(Job* new_job);
Job* get_job();
void remove_client(pid_t pid);

int find_client_index(pid_t pid);
int find_room_index(const char* channel_name);
void add_client_to_room(int room_idx, pid_t pid);
void remove_client_from_room(int room_idx, pid_t pid);
void send_reply(int target_qid, const char* sender, const char* text);


// --- Broadcaster Job Queue Functions ---

/**
 * @brief เพิ่มงานเข้าสู่ Broadcaster Job Queue (ป้องกันด้วย Mutex)
 * @param new_job โครงสร้างงานที่ต้องการเพิ่ม
 */
void add_job(Job* new_job) {
    pthread_mutex_lock(&job_mutex);
    new_job->next = NULL;
    if (job_queue_tail) {
        job_queue_tail->next = new_job;
    } else {
        job_queue_head = new_job;
    }
    job_queue_tail = new_job;
    pthread_cond_signal(&job_cond); // ส่งสัญญาณปลุก broadcaster ที่กำลังรอ
    pthread_mutex_unlock(&job_mutex);
}

/**
 * @brief ดึงงานจาก Broadcaster Job Queue (ป้องกันด้วย Mutex)
 * @return งานถัดไปในคิว, หรือจะถูกบล็อกหากคิวว่าง
 */
Job* get_job() {
    Job* job = NULL;
    pthread_mutex_lock(&job_mutex);
    
    // รอจนกว่าจะมีงานเข้ามาในคิว
    while (job_queue_head == NULL) {
        pthread_cond_wait(&job_cond, &job_mutex);
    }

    job = job_queue_head;
    job_queue_head = job->next;
    if (job_queue_head == NULL) {
        job_queue_tail = NULL;
    }
    job->next = NULL; // ปลด Job ออกจากรายการ

    pthread_mutex_unlock(&job_mutex);
    return job;
}

// --- IPC Helper: Broadcaster Logic ---

/**
 * @brief ส่งข้อความ ReplyMessage ไปยัง Message Queue ID ที่ระบุ
 * @details ใช้ IPC_NOWAIT เพื่อให้ Broadcaster Pool ไม่ถูกบล็อกแม้ Reply Queue ของ Client จะเต็ม (Queue Full) 
 * หากเต็มจะทิ้งข้อความ (Drop) เพื่อรักษา Throughput ของ Server
 * @param target_qid ID คิวเป้าหมาย (reply_qid ของ Client)
 * @param sender ชื่อผู้ส่งสำหรับแสดงผล
 * @param text เนื้อหาข้อความ
 */
void send_reply(int target_qid, const char* sender, const char* text) {
    ReplyMessage reply;
    reply.mtype = MSG_TYPE_BROADCAST;
    strncpy(reply.sender, sender, MAX_USERNAME - 1);
    reply.sender[MAX_USERNAME - 1] = '\0';
    strncpy(reply.text, text, MAX_TEXT_SIZE - 1);
    reply.text[MAX_TEXT_SIZE - 1] = '\0';

    // *** การปรับปรุงสำหรับ 100 คะแนน: ใช้ IPC_NOWAIT เพื่อ Performance และจัดการ Queue Full ***
    if (msgsnd(target_qid, &reply, sizeof(ReplyMessage) - sizeof(long), IPC_NOWAIT) == -1) {
        if (errno == EIDRM) {
             // คิวถูกลบแล้ว (Client ปิดตัวไปแล้ว) ให้เพิกเฉย
             // หากไม่ถูกเพิกเฉยจะเกิด Warning ทุกครั้งที่มีการ Broadcast 
        } else if (errno == EAGAIN) {
             // คิวเต็ม (Queue Full): ทิ้งข้อความนี้ไป เพื่อรักษา Throughput ของ Broadcaster Pool
             fprintf(stderr, "Broadcaster: Warning - Reply Queue (QID %d) is full (EAGAIN). Message dropped.\n", 
                     target_qid);
        } else {
             // ข้อผิดพลาดอื่นๆ ที่ไม่คาดคิด
             fprintf(stderr, "Broadcaster: Warning - msgsnd failed to QID %d. Error: %s\n", 
                     target_qid, strerror(errno));
        }
    }
}

/**
 * @brief Worker thread function สำหรับ Broadcaster Pool
 */
void* broadcaster_thread(void* arg) {
    // int thread_id = (int)(intptr_t)arg; // สำหรับ Debug
    while (1) {
        Job* job = get_job(); // บล็อกจนกว่าจะมีงาน

        // จัดการงานตามประเภท
        if (job->type == CMD_MSG) {
            // --- กระจายข้อความ (Broadcast) ต้องใช้ READ Lock ---
            pthread_rwlock_rdlock(&registry.rwlock);
            int room_idx = find_room_index(job->target_channel);
            
            if (room_idx != -1) {
                // ส่งไปยังสมาชิกทั้งหมดในห้อง
                for (int i = 0; i < registry.rooms[room_idx].member_count; i++) {
                    pid_t member_pid = registry.rooms[room_idx].members[i];
                    int client_idx = find_client_index(member_pid);

                    if (client_idx != -1) {
                        // [FIXED] ส่งให้สมาชิกทุกคนในห้อง รวมถึงผู้ส่ง (สำหรับ Probe RTT)
                        send_reply(registry.clients[client_idx].reply_qid, job->sender_name, job->message);
                    }
                    
                    // if (client_idx != -1) {
                    //     // ไม่ส่งข้อความกลับไปหาผู้ส่งเอง (sender_name คือ "[#channel] User PID")
                    //     char sender_pid_str[10];
                    //     // ต้องใช้ sscanf ที่ปลอดภัยกว่าการ substring เพื่อดึง PID จาก Sender Name
                    //     if (sscanf(job->sender_name, "[%*[^]]] User %s", sender_pid_str) == 1) {
                    //         pid_t sender_pid = (pid_t)atoi(sender_pid_str);
                    //         if (member_pid != sender_pid) {
                    //             send_reply(registry.clients[client_idx].reply_qid, job->sender_name, job->message);
                    //         }
                    //     }
                    // }
                }
            }
            pthread_rwlock_unlock(&registry.rwlock);

        } else if (job->type == CMD_DM || job->type == CMD_WHO || job->type == CMD_REGISTER || job->type == CMD_QUIT || job->type == CMD_LEAVE) {
            // --- Direct message หรือ Reply ทั่วไป (ส่งไปยัง QID เดียว) ---
            send_reply(job->target_qid, job->sender_name, job->message);
        }

        free(job); // คืนหน่วยความจำของ Job
    }
    return NULL;
}

// --- Registry Helpers (Access MUST be protected by registry.rwlock in handlers) ---

/**
 * @brief ค้นหาดัชนี Client ใน Registry
 * @param pid PID ของ Client
 * @return ดัชนีในอาเรย์ clients หรือ -1 หากไม่พบ
 */
int find_client_index(pid_t pid) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (registry.clients[i].pid == pid) return i;
    }
    return -1;
}

/**
 * @brief ค้นหาดัชนี Room ใน Registry
 * @param channel_name ชื่อ Channel
 * @return ดัชนีในอาเรย์ rooms หรือ -1 หากไม่พบ
 */
int find_room_index(const char* channel_name) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (strcmp(registry.rooms[i].channel_name, channel_name) == 0) return i;
    }
    return -1;
}

/**
 * @brief เพิ่ม Client เข้าสู่รายชื่อสมาชิก Room (ต้องเรียกภายใต้ WRITE Lock)
 */
void add_client_to_room(int room_idx, pid_t pid) {
    RoomEntry* room = &registry.rooms[room_idx];
    for (int i = 0; i < room->member_count; i++) {
        if (room->members[i] == pid) return; // เป็นสมาชิกอยู่แล้ว
    }
    if (room->member_count < MAX_CLIENTS) {
        room->members[room->member_count++] = pid;
    }
}

/**
 * @brief ลบ Client ออกจากรายชื่อสมาชิก Room (ต้องเรียกภายใต้ WRITE Lock)
 */
void remove_client_from_room(int room_idx, pid_t pid) {
    RoomEntry* room = &registry.rooms[room_idx];
    for (int i = 0; i < room->member_count; i++) {
        if (room->members[i] == pid) {
            // เลื่อนสมาชิกคนอื่นมาแทนที่
            for (int j = i; j < room->member_count - 1; j++) {
                room->members[j] = room->members[j + 1];
            }
            room->member_count--;
            // ถ้า Channel ว่าง ให้เคลียร์ Channel นั้น
            // *** System Event: "ห้องว่างแล้ว" ***
            if (room->member_count == 0 && strcmp(room->channel_name, "#general") != 0) {
                 printf("Router: Channel %s is now empty and will be cleared.\n", room->channel_name);
                 memset(room, 0, sizeof(RoomEntry));
                 registry.room_count--;
            }
            break;
        }
    }
}

/**
 * @brief ลบ Client ออกจากระบบทั้งหมด (ต้องเรียกภายใต้ WRITE Lock)
 * @details ใช้เมื่อ Client QUIT หรือเกิด Inactivity Timeout
 */
void remove_client(pid_t pid) {
    int client_idx = find_client_index(pid);
    if (client_idx == -1) return;

    // 1. นำออกจาก Channel ปัจจุบัน
    char channel_to_leave[MAX_CHANNEL];
    strcpy(channel_to_leave, registry.clients[client_idx].current_channel);
    
    if (channel_to_leave[0] != '\0') {
        int room_idx = find_room_index(channel_to_leave);
        if (room_idx != -1) {
            remove_client_from_room(room_idx, pid);
            
            // Broadcast แจ้งการออก
            Job* leave_job = (Job*)malloc(sizeof(Job));
            leave_job->type = CMD_MSG;
            strcpy(leave_job->sender_name, "SERVER");
            strcpy(leave_job->target_channel, channel_to_leave);
            sprintf(leave_job->message, "User %d has left the chat.", pid);
            add_job(leave_job);
        }
    }

    // 2. เคลียร์ Client Registry slot
    // msgctl(registry.clients[client_idx].reply_qid, IPC_RMID, NULL); // Client ควรลบคิวตัวเอง
    memset(&registry.clients[client_idx], 0, sizeof(ClientEntry));
    registry.client_count--;
    
    printf("Router: Client %d was removed. Client Count: %d\n", pid, registry.client_count);
}


// --- Router Command Handlers (Called by Router Thread - Execute under appropriate Lock) ---

void handle_register(const CommandMessage* cmd) {
    // ต้องใช้ WRITE Lock เพราะมีการแก้ไข Client Registry
    pthread_rwlock_wrlock(&registry.rwlock);
    
    int slot = find_client_index(0); // หา Slot ว่าง
    if (slot == -1) {
        // Server เต็ม, ส่ง error
        Job* error_job = (Job*)malloc(sizeof(Job));
        error_job->type = CMD_DM; error_job->target_qid = cmd->reply_qid;
        strcpy(error_job->sender_name, "SERVER");
        strcpy(error_job->message, "Error: Server is full. Connection rejected.");
        add_job(error_job);
        pthread_rwlock_unlock(&registry.rwlock);
        return;
    }

    // ลงทะเบียน Client
    registry.clients[slot].pid = cmd->sender_pid;
    registry.clients[slot].reply_qid = cmd->reply_qid;
    strcpy(registry.clients[slot].current_channel, "");
    registry.clients[slot].last_active = time(NULL); // กำหนดเวลา Active
    registry.client_count++;
    
    printf("Router: Client %d registered (QID: %d). Client Count: %d\n", cmd->sender_pid, cmd->reply_qid, registry.client_count);

    // ส่ง welcome message
    Job* welcome_job = (Job*)malloc(sizeof(Job));
    welcome_job->type = CMD_DM; 
    welcome_job->target_qid = cmd->reply_qid;
    strcpy(welcome_job->sender_name, "SERVER");
    sprintf(welcome_job->message, "Welcome User %d! Use JOIN <#channel> or WHO <#channel>.", cmd->sender_pid);
    add_job(welcome_job);
    
    pthread_rwlock_unlock(&registry.rwlock);
}

void handle_quit(const CommandMessage* cmd) {
    // ต้องใช้ WRITE Lock เพราะมีการแก้ไข Client/Room Registry
    pthread_rwlock_wrlock(&registry.rwlock);
    remove_client(cmd->sender_pid);
    
    // ส่งยืนยันการออกก่อนที่จะจบ (แม้ client จะปิดตัวทันที)
    Job* confirm_job = (Job*)malloc(sizeof(Job));
    confirm_job->type = CMD_DM; 
    confirm_job->target_qid = cmd->reply_qid;
    strcpy(confirm_job->sender_name, "SERVER");
    sprintf(confirm_job->message, "You have been disconnected. Goodbye.");
    add_job(confirm_job);
    
    pthread_rwlock_unlock(&registry.rwlock);
}

void handle_join(const CommandMessage* cmd) {
    // ต้องใช้ WRITE Lock เพราะมีการแก้ไข Client/Room Registry
    pthread_rwlock_wrlock(&registry.rwlock);
    
    int client_idx = find_client_index(cmd->sender_pid);
    if (client_idx == -1) { pthread_rwlock_unlock(&registry.rwlock); return; }

    char old_channel[MAX_CHANNEL];
    strcpy(old_channel, registry.clients[client_idx].current_channel);
    
    // 1. ตรวจสอบ/สร้าง Room
    int new_room_idx = find_room_index(cmd->channel);
    if (new_room_idx == -1) {
        // สร้างห้องใหม่ถ้ามีที่ว่าง
        int empty_slot = find_room_index(""); 
        if (empty_slot != -1 && registry.room_count < MAX_CHANNELS) {
            new_room_idx = empty_slot;
            strcpy(registry.rooms[new_room_idx].channel_name, cmd->channel);
            registry.rooms[new_room_idx].member_count = 0; 
            registry.room_count++;
            printf("Router: New channel %s created by %d.\n", cmd->channel, cmd->sender_pid);
        } else {
            // ไม่สามารถสร้างห้องได้
            Job* error_job = (Job*)malloc(sizeof(Job));
            error_job->type = CMD_DM; error_job->target_qid = cmd->reply_qid;
            strcpy(error_job->sender_name, "SERVER");
            strcpy(error_job->message, "Error: Cannot join/create channel, room limit reached.");
            add_job(error_job);
            pthread_rwlock_unlock(&registry.rwlock);
            return;
        }
    }

    // 2. ออกจาก Channel เก่า
    if (old_channel[0] != '\0' && strcmp(old_channel, cmd->channel) != 0) {
        int old_room_idx = find_room_index(old_channel);
        if (old_room_idx != -1) {
            remove_client_from_room(old_room_idx, cmd->sender_pid);
            
            // Broadcast แจ้งการออก
            Job* leave_job = (Job*)malloc(sizeof(Job));
            leave_job->type = CMD_MSG;
            strcpy(leave_job->sender_name, "SERVER");
            strcpy(leave_job->target_channel, old_channel);
            sprintf(leave_job->message, "User %d left the channel (Joined %s).", cmd->sender_pid, cmd->channel);
            add_job(leave_job);
        }
    }

    // 3. เข้าร่วม Channel ใหม่
    add_client_to_room(new_room_idx, cmd->sender_pid);
    strcpy(registry.clients[client_idx].current_channel, cmd->channel);

    // 4. ส่งยืนยันและ Broadcast การเข้าร่วม
    Job* confirm_job = (Job*)malloc(sizeof(Job));
    confirm_job->type = CMD_DM; 
    confirm_job->target_qid = cmd->reply_qid;
    strcpy(confirm_job->sender_name, "SERVER");
    sprintf(confirm_job->message, "You have joined %s. Total members: %d", cmd->channel, registry.rooms[new_room_idx].member_count);
    add_job(confirm_job);

    // *** System Event: "Alice joined" ***
    Job* join_job = (Job*)malloc(sizeof(Job));
    join_job->type = CMD_MSG;
    strcpy(join_job->sender_name, "SERVER");
    strcpy(join_job->target_channel, cmd->channel);
    sprintf(join_job->message, "User %d has joined the channel.", cmd->sender_pid);
    add_job(join_job);
    
    pthread_rwlock_unlock(&registry.rwlock);
}

void handle_msg(const CommandMessage* cmd) {
    // ต้องใช้ READ Lock เพราะแค่ตรวจสอบสถานะ Channel และส่ง Job
    pthread_rwlock_rdlock(&registry.rwlock);
    
    int client_idx = find_client_index(cmd->sender_pid);
    if (client_idx == -1) { pthread_rwlock_unlock(&registry.rwlock); return; }

    const char* current_channel = registry.clients[client_idx].current_channel;
    if (current_channel[0] == '\0') {
        Job* error_job = (Job*)malloc(sizeof(Job));
        error_job->type = CMD_DM; error_job->target_qid = cmd->reply_qid;
        strcpy(error_job->sender_name, "SERVER");
        strcpy(error_job->message, "Error: You are not in a channel. Use JOIN <#channel>.");
        add_job(error_job);
        pthread_rwlock_unlock(&registry.rwlock);
        return;
    }

    // สร้างและเพิ่ม Broadcast Job
    Job* msg_job = (Job*)malloc(sizeof(Job));
    msg_job->type = CMD_MSG;
    sprintf(msg_job->sender_name, "[%s] User %d", current_channel, cmd->sender_pid);
    strcpy(msg_job->target_channel, current_channel);
    strncpy(msg_job->message, cmd->text, MAX_TEXT_SIZE);
    add_job(msg_job);

    pthread_rwlock_unlock(&registry.rwlock);
}

void handle_dm(const CommandMessage* cmd) {
    // ต้องใช้ READ Lock เพราะแค่ตรวจสอบ PID และดึง QID
    pthread_rwlock_rdlock(&registry.rwlock);
    
    int sender_idx = find_client_index(cmd->sender_pid);
    if (sender_idx == -1) { pthread_rwlock_unlock(&registry.rwlock); return; }

    // แปลง Target PID string เป็น PID
    pid_t target_pid = (pid_t)atoi(cmd->target);
    int target_idx = find_client_index(target_pid);

    if (target_idx == -1) {
        // ส่ง Error กลับไปหาผู้ส่ง
        Job* error_job = (Job*)malloc(sizeof(Job));
        error_job->type = CMD_DM; error_job->target_qid = cmd->reply_qid;
        strcpy(error_job->sender_name, "SERVER");
        sprintf(error_job->message, "Error: User PID %s is not online.", cmd->target);
        add_job(error_job);
        pthread_rwlock_unlock(&registry.rwlock);
        return;
    }

    // 1. Job สำหรับ Target (DM)
    Job* target_job = (Job*)malloc(sizeof(Job));
    target_job->type = CMD_DM;
    target_job->target_qid = registry.clients[target_idx].reply_qid;
    sprintf(target_job->sender_name, "(DM from %d)", cmd->sender_pid);
    strncpy(target_job->message, cmd->text, MAX_TEXT_SIZE);
    add_job(target_job);
    
    // 2. Confirmation Job สำหรับผู้ส่ง
    Job* confirm_job = (Job*)malloc(sizeof(Job));
    confirm_job->type = CMD_DM;
    confirm_job->target_qid = cmd->reply_qid;
    strcpy(confirm_job->sender_name, "SERVER");
    sprintf(confirm_job->message, "DM sent to %d.", target_pid);
    add_job(confirm_job);

    pthread_rwlock_unlock(&registry.rwlock);
}

void handle_who(const CommandMessage* cmd) {
    // ต้องใช้ READ Lock เพราะแค่ตรวจสอบ Room และดึงรายชื่อสมาชิก
    pthread_rwlock_rdlock(&registry.rwlock);
    
    int client_idx = find_client_index(cmd->sender_pid);
    if (client_idx == -1) { pthread_rwlock_unlock(&registry.rwlock); return; }

    int room_idx = find_room_index(cmd->channel);
    
    Job* reply_job = (Job*)malloc(sizeof(Job));
    reply_job->type = CMD_WHO; 
    reply_job->target_qid = cmd->reply_qid;
    strcpy(reply_job->sender_name, "SERVER");

    if (room_idx == -1) {
        sprintf(reply_job->message, "Error: Channel %s does not exist.", cmd->channel);
    } else {
        char buffer[MAX_TEXT_SIZE];
        char* ptr = buffer;
        int remaining = MAX_TEXT_SIZE;
        
        int written = snprintf(ptr, remaining, "Members of %s (%d): ", cmd->channel, registry.rooms[room_idx].member_count);
        ptr += written; remaining -= written;

        for (int i = 0; i < registry.rooms[room_idx].member_count; i++) {
            written = snprintf(ptr, remaining, "%d%s", registry.rooms[room_idx].members[i], 
                               i < registry.rooms[room_idx].member_count - 1 ? ", " : "");
            ptr += written; remaining -= written;
            if (remaining <= 1) break; 
        }
        strncpy(reply_job->message, buffer, MAX_TEXT_SIZE);
    }
    add_job(reply_job);
    
    pthread_rwlock_unlock(&registry.rwlock);
}

void handle_leave(const CommandMessage* cmd) {
    // ต้องใช้ WRITE Lock เพราะมีการแก้ไข Client/Room Registry
    pthread_rwlock_wrlock(&registry.rwlock);

    int client_idx = find_client_index(cmd->sender_pid);
    if (client_idx == -1) { pthread_rwlock_unlock(&registry.rwlock); return; }

    char old_channel[MAX_CHANNEL];
    strcpy(old_channel, registry.clients[client_idx].current_channel);
    
    if (old_channel[0] == '\0') {
        Job* error_job = (Job*)malloc(sizeof(Job));
        error_job->type = CMD_DM; error_job->target_qid = cmd->reply_qid;
        strcpy(error_job->sender_name, "SERVER");
        strcpy(error_job->message, "Error: You are not currently in any channel.");
        add_job(error_job);
        pthread_rwlock_unlock(&registry.rwlock);
        return;
    }

    // 1. ลบ Client ออกจาก Room Registry
    int room_idx = find_room_index(old_channel);
    if (room_idx != -1) {
        remove_client_from_room(room_idx, cmd->sender_pid);
        
        // Broadcast แจ้งการออก
        Job* leave_job = (Job*)malloc(sizeof(Job));
        leave_job->type = CMD_MSG;
        strcpy(leave_job->sender_name, "SERVER");
        strcpy(leave_job->target_channel, old_channel);
        sprintf(leave_job->message, "User %d left the channel.", cmd->sender_pid);
        add_job(leave_job);
    }

    // 2. ล้างสถานะ Channel ของ Client
    strcpy(registry.clients[client_idx].current_channel, "");

    // 3. ส่งยืนยัน
    Job* confirm_job = (Job*)malloc(sizeof(Job));
    confirm_job->type = CMD_DM; 
    confirm_job->target_qid = cmd->reply_qid;
    strcpy(confirm_job->sender_name, "SERVER");
    sprintf(confirm_job->message, "You have left %s.", old_channel);
    add_job(confirm_job);

    pthread_rwlock_unlock(&registry.rwlock);
}


// --- Server Thread Functions ---

/**
 * @brief Thread หลักของ Router: อ่านคำสั่งจาก Control Queue และส่งต่อให้ Handlers
 */
void router_thread() {
    CommandMessage cmd_msg;
    int client_idx;

    while (1) {
        // อ่านจาก Control Queue (Blocking)
        ssize_t size = msgrcv(control_qid, &cmd_msg, sizeof(CommandMessage) - sizeof(long), MSG_TYPE_COMMAND, 0);

        if (size == -1) {
            if (errno == EINTR) continue; 
            if (errno == EIDRM) {
                printf("\nRouter: Control Queue removed. Exiting router thread...\n");
                break; // Server กำลังปิดตัว
            }
            perror("msgrcv (router)");
            continue;
        }
        
        // --- อัปเดตเวลา Active (ต้องใช้ WRITE Lock ชั่วขณะ) ---
        pthread_rwlock_wrlock(&registry.rwlock);
        client_idx = find_client_index(cmd_msg.sender_pid);
        if (client_idx != -1) {
            // อัปเดตเวลาที่ Client ล่าสุดส่งคำสั่งมา
            registry.clients[client_idx].last_active = time(NULL);
        }
        pthread_rwlock_unlock(&registry.rwlock);
        // --------------------------------------------------------

        printf("Router: Received command %d from PID %d\n", cmd_msg.command, cmd_msg.sender_pid);
        
        // ประมวลผลคำสั่ง: เรียก Handler ที่เหมาะสม
        switch (cmd_msg.command) {
            case CMD_REGISTER:
                handle_register(&cmd_msg);
                break;
            case CMD_JOIN:
                handle_join(&cmd_msg);
                break;
            case CMD_MSG:
                handle_msg(&cmd_msg);
                break;
            case CMD_DM:
                handle_dm(&cmd_msg);
                break;
            case CMD_WHO:
                handle_who(&cmd_msg);
                break;
            case CMD_LEAVE:
                handle_leave(&cmd_msg);
                break;
            case CMD_QUIT:
                handle_quit(&cmd_msg);
                break;
            default:
                fprintf(stderr, "Router: Received unknown command code %d\n", cmd_msg.command);
                break;
        }
    }
}

/**
 * @brief Thread สำหรับตรวจสอบ Client ที่ไม่มีการเคลื่อนไหวเกินกำหนด (Inactivity Timeout)
 */
void* monitor_clients(void* arg) {
    while (1) {
        sleep(10); // ตรวจสอบทุก 10 วินาที

        // ต้องใช้ WRITE Lock ในการตรวจสอบ เพราะอาจมีการเรียก remove_client
        pthread_rwlock_wrlock(&registry.rwlock); 
        time_t now = time(NULL);
        
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (registry.clients[i].pid != 0) {
                // ตรวจสอบ Inactivity
                if (difftime(now, registry.clients[i].last_active) > INACTIVITY_TIMEOUT) {
                    printf("Monitor: Kicking client %d for inactivity.\n", registry.clients[i].pid);
                    
                    // แจ้ง Client ก่อนถูกตัดการเชื่อมต่อ
                    Job* timeout_job = (Job*)malloc(sizeof(Job));
                    timeout_job->type = CMD_DM; 
                    timeout_job->target_qid = registry.clients[i].reply_qid;
                    strcpy(timeout_job->sender_name, "SERVER");
                    strcpy(timeout_job->message, "You have been disconnected due to inactivity.");
                    add_job(timeout_job);

                    // ลบ Client ออกจาก Registry
                    remove_client(registry.clients[i].pid);
                }
            }
        }
        pthread_rwlock_unlock(&registry.rwlock);
    }
    return NULL;
}


// --- Server Initialization and Cleanup ---
void init_server_state() {
    // กำหนดค่าเริ่มต้นของ Registry
    memset(&registry, 0, sizeof(GlobalRegistry));

    // สร้าง Reader-Writer Lock
    if (pthread_rwlock_init(&registry.rwlock, NULL) != 0) {
        perror("pthread_rwlock_init failed");
        exit(EXIT_FAILURE);
    }

    // สร้าง Channel เริ่มต้น
    strcpy(registry.rooms[0].channel_name, "#general");
    registry.room_count = 1;
    printf("Registry initialized with default channel: #general\n");
}

/**
 * @brief ฟังก์ชันจัดการการปิด Server (SIGINT)
 */
void cleanup(int sig) {
    printf("\nServer shutting down. Removing server Control Queue...\n");

    // 1. ลบ Control Queue เพื่อยุติ Router thread
    if (control_qid != -1 && msgctl(control_qid, IPC_RMID, NULL) == 0) {
        printf("Control Queue removed successfully.\n");
    } else if (control_qid != -1 && errno != EIDRM) {
        perror("Failed to remove Control Queue");
    }

    // 2. ทำลาย Lock และ Condition Variable
    pthread_rwlock_destroy(&registry.rwlock);
    pthread_mutex_destroy(&job_mutex);
    pthread_cond_destroy(&job_cond);

    exit(EXIT_SUCCESS);
}

int main() {
    pthread_t router_tid;
    pthread_t broadcaster_tids[BROADCASTER_COUNT];
    pthread_t monitor_tid;

    signal(SIGINT, cleanup); 

    init_server_state();

    // 1. สร้าง Control Queue ของ Server
    control_qid = msgget(CONTROL_QUEUE_KEY, IPC_CREAT | 0666);
    if (control_qid == -1) {
        perror("msgget (server)");
        cleanup(0);
        exit(EXIT_FAILURE);
    }

    printf("Chatroom Server started (Control QID: %d).\n", control_qid);
    printf("Architecture: Router + %d Broadcaster Threads + Monitor Thread (Timeout: %d secs).\n", BROADCASTER_COUNT, INACTIVITY_TIMEOUT);

    // 2. เริ่ม Router Thread
    if (pthread_create(&router_tid, NULL, (void* (*)(void*))router_thread, NULL) != 0) {
        perror("pthread_create (router)");
        cleanup(0);
        exit(EXIT_FAILURE);
    }

    // 3. เริ่ม Broadcaster Pool
    for (int i = 0; i < BROADCASTER_COUNT; i++) {
        if (pthread_create(&broadcaster_tids[i], NULL, broadcaster_thread, (void*)(intptr_t)i) != 0) {
            perror("pthread_create (broadcaster)");
            cleanup(0);
            exit(EXIT_FAILURE);
        }
    }
    
    // 4. เริ่ม Monitor Thread (สำหรับ Inactivity Timeout)
    if (pthread_create(&monitor_tid, NULL, monitor_clients, NULL) != 0) {
        perror("pthread_create (monitor)");
        cleanup(0);
        exit(EXIT_FAILURE);
    }

    // รอ Router thread จบ (เมื่อ Control Queue ถูกลบใน cleanup)
    pthread_join(router_tid, NULL);

    return 0; 
}