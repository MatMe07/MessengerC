#include <windows.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 4096

char my_private_slot[256];
char my_local_slot[256];
char server_slot[256];
char hostname[100];
int my_assigned_id = 0;

using namespace std;

BOOL dispatch_packet(const char* payload) {
    HANDLE hFile = CreateFile(server_slot, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        char raw_packet[BUFFER_SIZE];
        sprintf(raw_packet, "%s|%s", my_private_slot, payload);

        DWORD bytes_written;
        WriteFile(hFile, raw_packet, (DWORD)strlen(raw_packet), &bytes_written, NULL);
        CloseHandle(hFile);
        return TRUE; 
    }
    else {
        printf("[ОШИБКА] Не удалось связаться с сервером '%s'. Код: %d\n", server_slot, GetLastError());
        return FALSE;
    }
}

void clear_history() {
    HANDLE hHistoryFile = CreateFileA("client_history.txt", GENERIC_WRITE, 0, NULL,
        TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hHistoryFile == INVALID_HANDLE_VALUE) {
        hHistoryFile = CreateFileA("client_history.txt", GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    if (hHistoryFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hHistoryFile);
    }
}

void write_to_local_history(const char* message) {
    HANDLE hHistoryFile = CreateFileA("client_history.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hHistoryFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(hHistoryFile, 0, NULL, FILE_END);
        DWORD bytes_written;
        WriteFile(hHistoryFile, message, (DWORD)strlen(message), &bytes_written, NULL);
        WriteFile(hHistoryFile, "\r\n", 2, &bytes_written, NULL);
        CloseHandle(hHistoryFile);
    }
}

DWORD WINAPI response_listener(LPVOID lpParam) {
    HANDLE hMySlot = (HANDLE)lpParam;
    char receive_buffer[BUFFER_SIZE];
    DWORD bytes_read, next_message_size;

    while (1) {
        if (GetMailslotInfo(hMySlot, NULL, &next_message_size, NULL, NULL) && next_message_size != MAILSLOT_NO_MESSAGE) {
            memset(receive_buffer, 0, BUFFER_SIZE);
            if (ReadFile(hMySlot, receive_buffer, next_message_size, &bytes_read, NULL)) {
                receive_buffer[bytes_read] = '\0';

                if (strcmp(receive_buffer, "SHUTDOWN_OK") == 0) {
                    printf("\nСессия закрыта. Выход из приложения...\n");
                    break;
                }
                else if (strncmp(receive_buffer, "FILE_DATA:", 10) == 0) {
                    char* data = receive_buffer + 10;
                    char* separator = strchr(data, '|');

                    if (separator) {
                        *separator = '\0';
                        char* filename = data;
                        char* content = separator + 1;
                        char self_log_buf[BUFFER_SIZE];


                        HANDLE hFile = CreateFileA(filename, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

                        if (hFile != INVALID_HANDLE_VALUE) {
                            DWORD written;
                            WriteFile(hFile, content, (DWORD)strlen(content), &written, NULL);
                            CloseHandle(hFile);
                            sprintf(self_log_buf, "[Система] Файл '%s' получен от сервера (%d байт)\n> ", filename, written);
                            printf(self_log_buf);

                        }
                        else {
                            printf("\n[Ошибка] Не удалось сохранить файл '%s'\n> ", filename);
                            sprintf(self_log_buf, "\n[Ошибка] Не удалось сохранить файл '%s'\n> ", filename);
                            printf(self_log_buf);
                        }
                        write_to_local_history(self_log_buf);

                    }
                    fflush(stdout);
                    continue;
                }

                else if (strncmp(receive_buffer, "REG_OK:", 7) != 0) {
                    write_to_local_history(receive_buffer);

                    printf("\n%s\n> ", receive_buffer);
                }
                else {
                    my_assigned_id = atoi(receive_buffer + 7);
                    printf("\n[Система] Успешное подключение. Ваш ID = %d\n> ", my_assigned_id);
                }
                fflush(stdout);
            }
        }
        Sleep(40);
    }
    return 0;
}

int main() {
    setlocale(LC_ALL, "Russian");
    SetConsoleCP(1251);
    SetConsoleOutputCP(1251);
    clear_history();
    char serverHostName[100];
    char* newline;

    printf("Введите Host Name сервера > ");
    //cin >> serverHostName;
    fgets(serverHostName, sizeof(serverHostName), stdin);
    newline = strchr(serverHostName, '\n');
    if (newline) *newline = '\0';

    sprintf(server_slot, "\\\\%s\\mailslot\\server_main", serverHostName);

    int CurrProcessId = (int)GetCurrentProcessId();
    printf("Введите свой Host Name > ");
    fgets(hostname, sizeof(hostname), stdin);
    //cin >> hostname;
    newline = strchr(hostname, '\n');
    if (newline) *newline = '\0';

    sprintf(my_local_slot, "\\\\.\\mailslot\\client_");


    char pid_string[32];
    _itoa(CurrProcessId, pid_string, 10);
    strcat(my_local_slot, pid_string);


    sprintf(my_private_slot, "\\\\%s\\mailslot\\client_%d", hostname, CurrProcessId);


    HANDLE hMyMailslot = CreateMailslotA(my_local_slot, 0, MAILSLOT_WAIT_FOREVER, NULL);
    if (hMyMailslot == INVALID_HANDLE_VALUE) {
        printf("Не удалось инициализировать персональный клиентский ящик (%d)\n", GetLastError());
        return 1;
    }

    CreateThread(NULL, 0, response_listener, (LPVOID)hMyMailslot, 0, NULL);

    dispatch_packet("PING_INIT");
    printf("[Система] Синхронизация с сервером... Ожидание подтверждения сети...\n");

    int timeout_counter = 0;
    while (my_assigned_id == 0) {
        Sleep(100);
        timeout_counter++;

        if (timeout_counter >= 50) {
            printf("\n[КРИТИЧЕСКАЯ ОШИБКА] Сервер '%s' не ответил на запрос!\n", serverHostName);
            printf("Узел недоступен, либо введен неверный Host Name.\n");
            CloseHandle(hMyMailslot);
            system("pause");
            return 1;
        }
    }

    dispatch_packet("MSG:Зарегистрирован новый терминал клиента.");

    printf("Ваш уникальный почтовый ящик: %s\n", my_private_slot);
    printf("Доступный интерфейс команд:\n"
        "  <текст>                  - Отправить текстовое сообщение серверу\n"
        "  BROADCAST:<текст>        - Рассылка сообщения всем клиентам в сети\n"
        "  TASK:<имя_файла>         - Запрос серверу на подсчет пробелов в .txt файле\n"
        "  SEND:<имя_файла>         - Отправить файл на сервер\n"
        "  GET:<имя_файла>          - Скачать файл с сервера\n"
        "  HISTORY                  - Просмотреть историю переписки\n"
        "  exit                     - Безопасный выход из системы\n\n");

    char console_input[BUFFER_SIZE];
    while (1) {
        printf("> ");
        fgets(console_input, BUFFER_SIZE, stdin);
        if (strlen(console_input) == 0) {
            continue; 
        }
        size_t len = strlen(console_input);
        if (len > 0 && console_input[len - 1] == '\n') {
            console_input[len - 1] = '\0';
        }

        if (strcmp(console_input, "HISTORY") == 0) {
            HANDLE hHistoryFile = CreateFileA("client_history.txt", GENERIC_READ, FILE_SHARE_READ, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hHistoryFile != INVALID_HANDLE_VALUE) {
                char read_buffer[BUFFER_SIZE];
                DWORD file_bytes_read;

                printf("\n=== ЛОКАЛЬНАЯ ИСТОРИЯ ПЕРЕПИСКИ ===\n");
                while (ReadFile(hHistoryFile, read_buffer, BUFFER_SIZE - 1, &file_bytes_read, NULL) && file_bytes_read > 0) {
                    read_buffer[file_bytes_read] = '\0';
                    printf("%s", read_buffer);
                }
                printf("\n===================================\n");
                CloseHandle(hHistoryFile);
            }
            else {
                printf("[Система] История переписки пока пуста.\n");
            }
            continue;
        }
        else  if (strcmp(console_input, "exit") == 0) {
            dispatch_packet(console_input);
            Sleep(400);
            break;
        }
        else if (strncmp(console_input, "SEND:", 5) == 0) {
            char* filename = console_input + 5;
            char self_log_buf[BUFFER_SIZE];

            HANDLE hFile = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

            if (hFile != INVALID_HANDLE_VALUE) {
                char start_packet[BUFFER_SIZE];
                sprintf(start_packet, "START_SEND:%s", filename);
                dispatch_packet(start_packet);
                Sleep(20); 

                char file_chunk[1024];
                DWORD bytes_read;
                DWORD total_bytes_sent = 0;

                while (ReadFile(hFile, file_chunk, sizeof(file_chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
                    file_chunk[bytes_read] = '\0'; 

                    char chunk_packet[BUFFER_SIZE];
                    sprintf(chunk_packet, "CHUNKS_SEND:%s|%s", filename, file_chunk);
                    dispatch_packet(chunk_packet);

                    total_bytes_sent += bytes_read;

                    Sleep(30);
                }
                CloseHandle(hFile);

                char end_packet[BUFFER_SIZE];
                sprintf(end_packet, "END_SEND:%s", filename);
                dispatch_packet(end_packet);

                sprintf(self_log_buf, "[Система] Файл '%s' успешно отправлен по частям (%d байт)\n", filename, total_bytes_sent);
                write_to_local_history(self_log_buf);
                printf("%s", self_log_buf);
            }
            else {
                sprintf(self_log_buf, "[Ошибка] Файл '%s' не найден на клиенте\n", filename);
                write_to_local_history(self_log_buf);
                printf("%s", self_log_buf);
            }
            continue;
        }

        else if (strncmp(console_input, "GET:", 4) == 0) {
            char* filename = console_input + 4;

            dispatch_packet(console_input); 
            //Sleep(100000);

            continue;
        }
        else if (strlen(console_input) > 0) {
            char self_log_buf[BUFFER_SIZE];
            strcpy(self_log_buf, "[Вы]: ");
            strcat(self_log_buf, console_input);

            write_to_local_history(self_log_buf);

            dispatch_packet(console_input);
        }
        Sleep(50);
    }

    CloseHandle(hMyMailslot);
    return 0;
}
