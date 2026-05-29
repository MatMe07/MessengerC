#include <windows.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 4096
#define MAX_CLIENTS 20
int client_count = 0;

using namespace std;

typedef struct {
    int id;
    char reply_slot_name[256];
    int is_active;
} ClientInfo;

ClientInfo clients[MAX_CLIENTS];
int next_client_id = 1;

CRITICAL_SECTION cs_clients;
CRITICAL_SECTION cs_history;

void send_response(const char* client_slot, const char* message) {
    HANDLE hFile = CreateFileA(client_slot, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD bytes_written;
        WriteFile(hFile, message, (DWORD)strlen(message), &bytes_written, NULL);
        CloseHandle(hFile);
    }
}

void save_to_common_history(const char* log_line) {
    EnterCriticalSection(&cs_history);
    HANDLE hFile = CreateFileA("common_chat_history.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(hFile, 0, NULL, FILE_END);
        DWORD bytes_written;
        WriteFile(hFile, log_line, (DWORD)strlen(log_line), &bytes_written, NULL);
        WriteFile(hFile, "\r\n", 2, &bytes_written, NULL);
        CloseHandle(hFile);
    }
    printf("%s\n", log_line);
    LeaveCriticalSection(&cs_history);
}

DWORD WINAPI server_input_thread(LPVOID lpParam) {
    char input_buf[BUFFER_SIZE];
    char packet_to_send[BUFFER_SIZE];
    char log_buf[BUFFER_SIZE];

    while (1) {
        printf("> ");
        cin.getline(input_buf, BUFFER_SIZE);

        size_t len = strlen(input_buf);
        if (len > 0 && input_buf[len - 1] == '\n') {
            input_buf[len - 1] = '\0';
        }

        if (strlen(input_buf) == 0) continue;

        if (strcmp(input_buf, "LIST") == 0) {
            printf("\n=== СПИСОК АКТИВНЫХ КЛИЕНТОВ ===\n");
            int active_count = 0;
            EnterCriticalSection(&cs_clients);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].is_active) {
                    printf("ID: %d | Путь к Mailslot: %s\n", clients[i].id, clients[i].reply_slot_name);
                    active_count++;
                }
            }
            LeaveCriticalSection(&cs_clients);
            if (active_count == 0) printf("(Нет подключенных клиентов)\n");
            printf("=================================\n\n");
            continue;
        }

        char* colon = strchr(input_buf, ':');

        if (colon != NULL) {
            *colon = '\0';
            int target_id = atoi(input_buf);
            char* message_text = colon + 1;
            while (*message_text == ' ') message_text++;

            int found = 0;
            char target_slot[256] = { 0 };

            EnterCriticalSection(&cs_clients);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].is_active && clients[i].id == target_id) {
                    strcpy(target_slot, clients[i].reply_slot_name);
                    found = 1;
                    break;
                }
            }
            LeaveCriticalSection(&cs_clients);

            if (strncmp(input_buf, "ALL", 3) == 0) {
                sprintf(packet_to_send, "[Сервер (всем)]: %s", message_text);

                EnterCriticalSection(&cs_clients);
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].is_active) {
                        send_response(clients[i].reply_slot_name, packet_to_send);
                    }
                }
                LeaveCriticalSection(&cs_clients);

                sprintf(log_buf, "[Сервер -> ВСЕМ]: %s", message_text);
                save_to_common_history(log_buf);
            }
            else if (found) {
                sprintf(packet_to_send, "[Сервер]: %s", message_text);
                send_response(target_slot, packet_to_send);

                sprintf(log_buf, "[Сервер -> Клиент %d]: %s", target_id, message_text);
                save_to_common_history(log_buf);
            }
            else {
                printf("[Система] Клиент с ID %d не найден.\n", target_id);
            }
        }
        else {
            printf("Доступные команды сервера:\n"
                "  LIST                     - Показать список всех активных клиентов\n"
                "  ALL: текст               - Отправить сообщение ВСЕМ клиентам\n"
                "  ID: текст                - Отправить сообщение клиенту по его ID\n");
        }
    }
    return 0;
}

void process_variant_task(const char* filename, char* response) {
    HANDLE hFile = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        sprintf(response, "ERROR: Не удалось открыть файл '%s'. Проверьте путь.", filename);
        return;
    }

    char chunk[512];
    DWORD bytes_read;
    int space_count = 0;

    while (ReadFile(hFile, chunk, sizeof(chunk), &bytes_read, NULL) && bytes_read > 0) {
        for (DWORD i = 0; i < bytes_read; i++) {
            if (chunk[i] == ' ') {
                space_count++;
            }
        }
    }
    CloseHandle(hFile);
    sprintf(response, "RESULT: Количество пробелов в файле '%s' = %d", filename, space_count);
}

DWORD WINAPI client_worker(LPVOID lpParam) {
    char* raw_packet = (char*)lpParam;
    char* separator = strchr(raw_packet, '|');
    if (!separator) { free(raw_packet); return 0; }

    *separator = '\0';
    char* client_slot = raw_packet;
    char* command_data = separator + 1;

    int current_id = 0;

    EnterCriticalSection(&cs_clients);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].is_active && strcmp(clients[i].reply_slot_name, client_slot) == 0) {
            current_id = clients[i].id;
            break;
        }
    }

    if (current_id == 0) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].is_active) {
                clients[i].id = next_client_id++;
                strcpy(clients[i].reply_slot_name, client_slot);
                clients[i].is_active = 1;
                current_id = clients[i].id;
                client_count++;
                break;
            }
        }

        char welcome[256];
        sprintf(welcome, "REG_OK:%d", current_id);
        send_response(client_slot, welcome);

    }
    LeaveCriticalSection(&cs_clients);

    char log_buf[BUFFER_SIZE];

     if (strncmp(command_data, "GET:", 4) == 0) {
        char* filename = command_data + 4;
        printf("[Сервер] Клиент %d запросил файл: %s\n", current_id, filename);

        HANDLE hFile = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (hFile != INVALID_HANDLE_VALUE) {
            char file_content[BUFFER_SIZE];
            DWORD bytes_read;

            if (ReadFile(hFile, file_content, BUFFER_SIZE - 100, &bytes_read, NULL)) {
                file_content[bytes_read] = '\0';

                char response[BUFFER_SIZE + 200];
                sprintf(response, "FILE_DATA:%s|%s", filename, file_content);
                send_response(client_slot, response);
                //Sleep(1000);

            }
            CloseHandle(hFile);
            printf("[Сервер] Файл '%s' отправлен клиенту %d\n", filename, current_id);
        }
        else {
            send_response(client_slot, "ERROR: Файл не найден на сервере");
            printf("[Сервер] Файл '%s' не найден\n", filename);
        }

    }
    else if (strcmp(command_data, "exit") == 0) {
        sprintf(log_buf, "[СИСТЕМА] Клиент %d покинул чат.", current_id);
        save_to_common_history(log_buf);

        EnterCriticalSection(&cs_clients);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].is_active && clients[i].id == current_id) {
                clients[i].is_active = 0;
                client_count--;
                break;
            }
        }
        LeaveCriticalSection(&cs_clients);
        send_response(client_slot, "SHUTDOWN_OK");
    }
    else if (strncmp(command_data, "BROADCAST:", 10) == 0) {
        char broadcast_payload[BUFFER_SIZE];
        sprintf(broadcast_payload, "[Клиент %d]: %s", current_id, command_data + 10);
        save_to_common_history(broadcast_payload);

        EnterCriticalSection(&cs_clients);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].is_active && current_id != clients[i].id) {
                send_response(clients[i].reply_slot_name, broadcast_payload);
            }
        }
        LeaveCriticalSection(&cs_clients);
    }
    else if (strncmp(command_data, "TASK:", 5) == 0) {
        char file_response[512];
        char* filename = command_data + 5;

        sprintf(log_buf, "[ЗАПРОС] Клиент %d запросил анализ файла: %s", current_id, filename);
        save_to_common_history(log_buf);

        process_variant_task(filename, file_response);
        send_response(client_slot, file_response);
    }
    else if (strncmp(command_data, "SEND:", 5) == 0) {
        char* file_part = command_data + 5;
        char* separator = strchr(file_part, '|');

        if (separator) {
            *separator = '\0';
            char* filename = file_part;
            char* content = separator + 1;

            HANDLE hFile = CreateFileA(filename, GENERIC_WRITE, 0, NULL,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteFile(hFile, content, (DWORD)strlen(content), &written, NULL);
                CloseHandle(hFile);
                send_response(client_slot, "OK: Файл сохранен на сервере");

                char log_buf[BUFFER_SIZE];
                sprintf(log_buf, "[ФАЙЛ] Клиент %d загрузил файл '%s' (%d байт)",
                    current_id, filename, written);
                save_to_common_history(log_buf);
            }
            else {
                send_response(client_slot, "ERROR: Не удалось сохранить файл");
            }
        }
    }
    else if (strncmp(command_data, "START_SEND:", 11) == 0) {
         char* filename = command_data + 11;

         // CREATE_ALWAYS затрет старый файл или создаст новый пустой размером 0 байт
         HANDLE hFile = CreateFileA(filename, GENERIC_WRITE, 0, NULL,
             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
         if (hFile != INVALID_HANDLE_VALUE) {
             CloseHandle(hFile);
         }
     }
    else if (strncmp(command_data, "CHUNKS_SEND:", 12) == 0) {
         char* file_part = command_data + 12;
         char* pipe_separator = strchr(file_part, '|');

         if (pipe_separator) {
             *pipe_separator = '\0';
             char* filename = file_part;
             char* content_chunk = pipe_separator + 1;

             // OPEN_ALWAYS открывает файл, а FILE_APPEND_DATA переносит указатель в конец
             HANDLE hFile = CreateFileA(filename, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

             if (hFile != INVALID_HANDLE_VALUE) {
                 DWORD written;
                 // Дописываем кусочек данных в хвост файла
                 WriteFile(hFile, content_chunk, (DWORD)strlen(content_chunk), &written, NULL);
                 CloseHandle(hFile);
             }
         }
     }
    else if (strncmp(command_data, "END_SEND:", 9) == 0) {
         char* filename = command_data + 9;

         char server_log[BUFFER_SIZE];
         sprintf(server_log, "[ФАЙЛ] Клиент %d завершил загрузку файла '%s'", current_id, filename);
         save_to_common_history(server_log);

         send_response(client_slot, "OK: Файл успешно загружен и собран на сервере");
     }
    else {
        sprintf(log_buf, "Клиент %d: %s", current_id, command_data);
        save_to_common_history(log_buf);

        //EnterCriticalSection(&cs_clients);
        //for (int i = 0; i < MAX_CLIENTS; i++) {
        //    if (clients[i].is_active && clients[i].id != current_id) {
        //        send_response(clients[i].reply_slot_name, log_buf);
        //    }
        //}
        //LeaveCriticalSection(&cs_clients);
    }

    free(raw_packet);
    return 0;
}

int main() {
    setlocale(LC_ALL, "Russian");
    SetConsoleCP(1251);
    SetConsoleOutputCP(1251);

    InitializeCriticalSection(&cs_clients);
    InitializeCriticalSection(&cs_history);

    memset(clients, 0, sizeof(clients));

    HANDLE hServerMailslot = CreateMailslotA("\\\\.\\mailslot\\server_main", 0, MAILSLOT_WAIT_FOREVER, NULL);
    if (hServerMailslot == INVALID_HANDLE_VALUE) {
        printf("Ошибка запуска сервера (%d)\n", GetLastError());
        return 1;
    }

    printf("[СЕРВЕР ЧАТА] Запущен. Ожидание сообщений...\n");
    printf("Формат отправки клиентам: ID: текст (Пример: 1: Hello World)\n\n");

    CreateThread(NULL, 0, server_input_thread, NULL, 0, NULL);

    char incoming_buffer[BUFFER_SIZE];
    DWORD bytes_read, next_message_size;

    while (1) {
        if (GetMailslotInfo(hServerMailslot, NULL, &next_message_size, NULL, NULL) && next_message_size != MAILSLOT_NO_MESSAGE) {
            memset(incoming_buffer, 0, BUFFER_SIZE);
            if (ReadFile(hServerMailslot, incoming_buffer, next_message_size, &bytes_read, NULL)) {
                char* ThreadData = (char*)malloc(bytes_read + 1);
                memcpy(ThreadData, incoming_buffer, bytes_read);
                ThreadData[bytes_read] = '\0';

                CreateThread(NULL, 0, client_worker, (LPVOID)ThreadData, 0, NULL);
                //Sleep(1000);
            }
        }
        Sleep(20);
    }

    CloseHandle(hServerMailslot);
    return 0;
}