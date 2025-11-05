#ifndef PROJECT_DEFS_H
#define PROJECT_DEFS_H

#include <sys/types.h>
#include <pthread.h>
#include <time.h> // สำหรับ time_t

// --- IPC Keys & Types ---
#define CONTROL_QUEUE_KEY 1234
#define BROADCASTER_COUNT 4     // จำนวน worker thread ใน pool
#define MAX_TEXT_SIZE 256
#define MAX_CHANNEL 32
#define MAX_USERNAME 32
#define MAX_CLIENTS 10          
#define MAX_CHANNELS 5          
#define INACTIVITY_TIMEOUT 120 // 120 วินาที (2 นาที)

#define MSG_TYPE_COMMAND 1L     // Message type สำหรับคำสั่ง (Client -> Router)
#define MSG_TYPE_BROADCAST 2L   // Message type สำหรับข้อความตอบกลับ/กระจาย (Broadcaster -> Client)

// --- Command Codes (กำหนดโดย Client) ---
typedef enum {
    CMD_REGISTER,
    CMD_JOIN,
    CMD_MSG, // ใช้แทน SAY
    CMD_DM,
    CMD_WHO,
    CMD_LEAVE,
    CMD_QUIT,
} CommandCode;

// --- Message Structure (Client -> Router) ---
typedef struct {
    long mtype;             // ต้องเป็น MSG_TYPE_COMMAND (1)
    CommandCode command;
    pid_t sender_pid;       // PID ของ Client
    int reply_qid;          // ID คิวส่วนตัวของ Client (สำคัญมาก)
    char channel[MAX_CHANNEL]; // Channel เป้าหมายสำหรับ JOIN/MSG/WHO
    char target[MAX_USERNAME]; // Target PID string สำหรับ DM
    char text[MAX_TEXT_SIZE];  // เนื้อหาข้อความ
} CommandMessage;

// --- Message Structure (Broadcaster -> Client) ---
typedef struct {
    long mtype;             // ต้องเป็น MSG_TYPE_BROADCAST (2)
    char sender[MAX_USERNAME]; // ชื่อผู้ส่งที่ถูกจัดรูปแบบแล้ว (เช่น "[#room] User 12345")
    char text[MAX_TEXT_SIZE];  // เนื้อหาข้อความ
} ReplyMessage;

// --- Broadcaster Job Structure ---
// โครงสร้างงานที่ Router ส่งให้ Broadcaster Pool
typedef struct Job {
    CommandCode type;
    char sender_name[MAX_USERNAME]; // ชื่อผู้ส่งที่ใช้แสดงผล
    char target_channel[MAX_CHANNEL]; // Channel ที่ต้อง Broadcast
    int target_qid;                 // Specific QID สำหรับ DM หรือ Reply
    char message[MAX_TEXT_SIZE];
    struct Job *next;
} Job;

// --- Server Registry Data Structures ---

// Client Registry Entry (PID -> QID + Channel + Last Active Time)
typedef struct {
    pid_t pid;
    int reply_qid;
    char current_channel[MAX_CHANNEL]; 
    time_t last_active; // เวลาล่าสุดที่ Client ส่งคำสั่งมา
} ClientEntry;

// Room Registry Entry (Channel -> List of PIDs)
typedef struct {
    char channel_name[MAX_CHANNEL];
    pid_t members[MAX_CLIENTS]; 
    int member_count;
} RoomEntry;

// Global Registry State Structure (มี Lock ป้องกัน)
typedef struct {
    ClientEntry clients[MAX_CLIENTS];
    int client_count;
    
    RoomEntry rooms[MAX_CHANNELS];
    int room_count;

    pthread_rwlock_t rwlock; // Reader-Writer Lock สำหรับป้องกันการเข้าถึง registries
} GlobalRegistry;

#endif // PROJECT_DEFS_H