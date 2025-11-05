#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
// รวมไฟล์ header ที่กำหนดโครงสร้างและค่าคงที่ทั้งหมด
#include "project_defs.h" 

// --- GLOBAL STATE ---
int control_qid = -1; // คิวควบคุมหลักของ Server (CONTROL_QUEUE_KEY)
int reply_qid = -1;   // คิวส่วนตัวของ Client (IPC_PRIVATE)
pid_t client_pid;    // PID ของไคลเอนต์เอง

// --- FORWARD DECLARATIONS ---
void cleanup(int sig);
void* sender_thread(void* arg);
void* receiver_thread(void* arg);
void send_command(CommandCode command, const char* channel, const char* target, const char* text);

// --- THREAD 1: SENDER (Reads stdin and sends commands) ---
void* sender_thread(void* arg) {
    char input_buffer[MAX_TEXT_SIZE + 100]; // Buffer for user input
    char cmd_str[20], param1[MAX_CHANNEL], text_content[MAX_TEXT_SIZE];

    printf("Enter commands (e.g., JOIN #room, MSG <text>, DM <PID> <text>, WHO #room, QUIT):\n> ");

    while (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
        // Clear previous values
        cmd_str[0] = param1[0] = text_content[0] = '\0';
        
        // Remove trailing newline
        input_buffer[strcspn(input_buffer, "\n")] = 0;

        // Basic command parsing: CMD [PARAM1] [TEXT...]
        // sscanf returns the number of successfully matched items
        if (sscanf(input_buffer, "%19s %31s %[^\n]", cmd_str, param1, text_content) < 1) {
            printf("> ");
            continue; // Empty line
        }
        
        // Convert command string to CommandCode and send
        if (strcmp(cmd_str, "JOIN") == 0 && param1[0] != '\0') {
            send_command(CMD_JOIN, param1, "", "");
        } else if (strcmp(cmd_str, "MSG") == 0 && text_content[0] != '\0') {
            send_command(CMD_MSG, "", "", text_content);
        } else if (strcmp(cmd_str, "DM") == 0 && param1[0] != '\0' && text_content[0] != '\0') {
            // Note: In the server, target needs to be resolved from string/PID to reply_qid
            send_command(CMD_DM, "", param1, text_content);
        } else if (strcmp(cmd_str, "WHO") == 0 && param1[0] != '\0') {
            send_command(CMD_WHO, param1, "", "");
        } else if (strcmp(cmd_str, "LEAVE") == 0) {
            send_command(CMD_LEAVE, "", "", "");
        } else if (strcmp(cmd_str, "QUIT") == 0) {
            send_command(CMD_QUIT, "", "", "Goodbye");
            kill(client_pid, SIGINT); // Trigger cleanup and exit via signal handler
            break;
        } else {
            printf("Unknown command or missing parameters. Please retry.\n> ");
            continue;
        }
        printf("> ");
        fflush(stdout);
    }
    
    return NULL;
}

// --- THREAD 2: RECEIVER (Blocks on Reply Queue) ---
void* receiver_thread(void* arg) {
    ReplyMessage reply;

    while (1) {
        // รอรับข้อความ mtype 2 (MSG_TYPE_BROADCAST) บน queue ส่วนตัว
        if (msgrcv(reply_qid, &reply, sizeof(ReplyMessage) - sizeof(long), MSG_TYPE_BROADCAST, 0) == -1) {
            if (errno == EIDRM) {
                // Queue ถูกลบแล้ว (server หรือตัว client เองเป็นคนลบ)
                printf("\nServer disconnected or Private Queue removed. Exiting receiver thread...\n");
                break;
            }
            perror("msgrcv (client)");
            continue;
        }

        // แสดงผลลัพธ์: \r (carriage return) ใช้สำหรับเคลียร์บรรทัดที่กำลังพิมพ์
        printf("\r[%s] %s\n> ", reply.sender, reply.text);
        fflush(stdout); // แสดงผลทันที
    }
    
    return NULL;
}

// --- IPC HELPER (ส่งคำสั่งไปยัง Server Control Queue) ---
void send_command(CommandCode command, const char* channel, const char* target, const char* text) {
    CommandMessage cmd;
    cmd.mtype = MSG_TYPE_COMMAND;
    cmd.command = command;
    cmd.sender_pid = client_pid;
    cmd.reply_qid = reply_qid; // **ส่ง ID คิวส่วนตัวไปให้ Server**
    strncpy(cmd.channel, channel, MAX_CHANNEL);
    strncpy(cmd.target, target, MAX_USERNAME);
    strncpy(cmd.text, text, MAX_TEXT_SIZE);

    // msgsnd sends non-blocking, we assume the server's control queue is large enough
    if (msgsnd(control_qid, &cmd, sizeof(CommandMessage) - sizeof(long), 0) == -1) {
        if (errno == EIDRM) {
            fprintf(stderr, "\nERROR: Server Control Queue was removed. Exiting...\n");
            cleanup(0);
        } else {
            perror("msgsnd to Control Queue failed");
        }
    }
}

// --- CLEANUP FUNCTION (Ctrl+C, QUIT, หรือ SIGTERM) ---
void cleanup(int sig) {
    if (sig == SIGTERM) {
         printf("\nReceived SIGTERM from monitor thread. Removing client queue...\n");
    } else if (sig == SIGINT) {
        printf("\nReceived SIGINT. Shutting down...\n");
    } else {
        printf("\nClient shutting down normally. Removing client queue...\n");
    }

    // Remove the private Reply Queue (ป้องกันการค้าง)
    if (reply_qid != -1 && msgctl(reply_qid, IPC_RMID, NULL) == 0) {
        printf("Private Reply Queue removed successfully.\n");
    } else if (reply_qid != -1 && errno != EIDRM) {
        perror("Failed to remove private Reply Queue");
    }
    
    exit(EXIT_SUCCESS);
}

// --- MAIN FUNCTION ---
int main() {
    pthread_t sender_tid, receiver_tid;

    client_pid = getpid();
    // ดัก SIGINT และ SIGTERM เพื่อลบคิวส่วนตัวก่อนออก
    signal(SIGINT, cleanup); 
    signal(SIGTERM, cleanup); 

    // 1. เชื่อมต่อ Queue ของ Server (Control Queue)
    control_qid = msgget(CONTROL_QUEUE_KEY, 0666);
    if (control_qid == -1) {
        perror("Failed to get Control Queue. Is server running?");
        exit(EXIT_FAILURE);
    }
    
    // 2. สร้าง Queue ส่วนตัว (Reply Queue) ใช้ IPC_PRIVATE เพื่อให้มี ID เฉพาะตัว
    reply_qid = msgget(IPC_PRIVATE, IPC_CREAT | 0666);
    if (reply_qid == -1) {
        perror("Failed to create private Reply Queue");
        exit(EXIT_FAILURE);
    }

    printf("Client started (PID: %d). Private Reply Queue ID: %d\n", client_pid, reply_qid);

    // 3. ส่งข้อความ REGISTER ไปยัง Server ทันที
    send_command(CMD_REGISTER, "", "", "New client connection");

    // 4. สร้าง 2 เธรด
    if (pthread_create(&receiver_tid, NULL, receiver_thread, NULL) != 0) {
        perror("pthread_create (receiver)");
        cleanup(0);
    }
    if (pthread_create(&sender_tid, NULL, sender_thread, NULL) != 0) {
        perror("pthread_create (sender)");
        cleanup(0);
    }

    // รอเธรดทำงานจบ (sender thread จะจบเมื่อผู้ใช้พิมพ์ QUIT)
    pthread_join(sender_tid, NULL);
    // เมื่อ sender thread จบ, receiver thread ก็จะจบตามการ cleanup()
    pthread_join(receiver_tid, NULL);

    // Cleanup (กรณีจบปกติ)
    cleanup(0);
    return 0;
}