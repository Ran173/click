#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <string.h>

/* ========== 配置常量 ========== */
#define MAX_RECORD_POINTS 10000
#define HOTKEY_TOGGLE     VK_F6
#define HOTKEY_RECORD     VK_F7
#define HOTKEY_EXIT       VK_F8
#define MENU_REFRESH_MS   200
#define INDICATOR_SIZE    36
#define INDICATOR_SHOW_MS 300

/* 自定义消息 */
#define WM_SHOW_INDICATOR (WM_USER + 1)

/* 界面布局行号 */
#define MENU_LINES        13
#define STATUS_LINE       (MENU_LINES)
#define SEP_LINE          (STATUS_LINE + 1)
#define OP_TITLE_LINE     (SEP_LINE + 1)
#define INPUT_LINE        (OP_TITLE_LINE + 1)
#define MSG_LINE          (INPUT_LINE + 1)

/* ========== 录制的点击事件 ========== */
typedef struct {
    POINT   pos;
    DWORD   button;
    DWORD   delay_ms;
} RecordEvent;

/* ========== 全局状态 ========== */
typedef struct {
    volatile int  running;
    volatile int  click_count;
    volatile int  clicked;
    volatile int  interval_ms;
    volatile int  mouse_button;

    volatile int  recording;
    volatile int  replaying;
    RecordEvent   records[MAX_RECORD_POINTS];
    volatile int  record_count;

    HANDLE        hClickThread;
    HANDLE        hRecordThread;
    HANDLE        hReplayThread;
    HANDLE        hIndicatorThread;
    volatile int  exit_flag;
} AppState;

static AppState g_state = {0};
static HWND    g_hIndicatorWnd = NULL;

/* ========== 辅助函数 ========== */
static DWORD button_to_down(DWORD btn) {
    switch (btn) {
        case 1:  return MOUSEEVENTF_RIGHTDOWN;
        case 2:  return MOUSEEVENTF_MIDDLEDOWN;
        default: return MOUSEEVENTF_LEFTDOWN;
    }
}

static DWORD button_to_up(DWORD btn) {
    switch (btn) {
        case 1:  return MOUSEEVENTF_RIGHTUP;
        case 2:  return MOUSEEVENTF_MIDDLEUP;
        default: return MOUSEEVENTF_LEFTUP;
    }
}

static void do_click_raw(int button) {
    INPUT inputs[2];
    memset(&inputs, 0, sizeof(inputs));
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = button_to_down(button);
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = button_to_up(button);
    SendInput(2, inputs, sizeof(INPUT));
}

/* 点击并在该位置显示指示器 */
static void do_click(int button) {
    POINT pt;
    GetCursorPos(&pt);
    do_click_raw(button);
    if (g_hIndicatorWnd)
        PostMessage(g_hIndicatorWnd, WM_SHOW_INDICATOR, pt.x, pt.y);
}

/* 在指定坐标点击并显示指示器（回放用） */
static void do_click_at(int x, int y, int button) {
    SetCursorPos(x, y);
    Sleep(5);
    do_click_raw(button);
    if (g_hIndicatorWnd)
        PostMessage(g_hIndicatorWnd, WM_SHOW_INDICATOR, x, y);
}

static void goto_xy(int x, int y) {
    COORD coord = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
}

static void clear_line(int y) {
    goto_xy(0, y);
    printf("%-78s", "");
}

static void clear_area(int from, int to) {
    int i;
    for (i = from; i <= to; i++) clear_line(i);
    goto_xy(0, from);
}

static int read_int_at(int line, const char* prompt, int* out) {
    char buf[64];
    clear_area(OP_TITLE_LINE, MSG_LINE);
    goto_xy(0, INPUT_LINE);
    printf("  %s", prompt);
    goto_xy((int)strlen(prompt) + 4, INPUT_LINE);
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    if (sscanf(buf, "%d", out) == 1) return 1;
    return 0;
}

static void show_msg(const char* msg) {
    clear_line(MSG_LINE);
    goto_xy(0, MSG_LINE);
    printf("  %s", msg);
}

static void set_cursor_visible(int visible) {
    CONSOLE_CURSOR_INFO ci;
    GetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci);
    ci.bVisible = visible;
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci);
}

/* ========== 点击位置指示器窗口 ========== */
LRESULT CALLBACK IndicatorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            /* 用定时器控制指示器自动隐藏 */
            SetTimer(hwnd, 1, INDICATOR_SHOW_MS, NULL);
            /* 初始隐藏 */
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        case WM_SHOW_INDICATOR: {
            /* 重置定时器，移到新位置并显示 */
            KillTimer(hwnd, 1);
            SetTimer(hwnd, 1, INDICATOR_SHOW_MS, NULL);
            int cx = (int)wParam - INDICATOR_SIZE / 2;
            int cy = (int)lParam - INDICATOR_SIZE / 2;
            SetWindowPos(hwnd, HWND_TOPMOST,
                cx, cy, INDICATOR_SIZE, INDICATOR_SIZE,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_TIMER:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            int s = INDICATOR_SIZE;

            /* 外圈：半透明红色填充 */
            HPEN hRedPen = CreatePen(PS_SOLID, 3, RGB(255, 60, 60));
            HBRUSH hRedBrush = CreateSolidBrush(RGB(255, 80, 80));
            HPEN hOldPen = SelectObject(hdc, hRedPen);
            HBRUSH hOldBrush = SelectObject(hdc, hRedBrush);
            Ellipse(hdc, 2, 2, s - 2, s - 2);

            /* 白色十字线 */
            SelectObject(hdc, GetStockObject(WHITE_PEN));
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            MoveToEx(hdc, s / 2, s / 4, NULL);
            LineTo(hdc, s / 2, s * 3 / 4);
            MoveToEx(hdc, s / 4, s / 2, NULL);
            LineTo(hdc, s * 3 / 4, s / 2);

            SelectObject(hdc, hOldPen);
            SelectObject(hdc, hOldBrush);
            DeleteObject(hRedPen);
            DeleteObject(hRedBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

DWORD WINAPI indicator_thread(LPVOID param) {
    HINSTANCE hInst = GetModuleHandle(NULL);

    WNDCLASSEX wc = {0};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.lpfnWndProc   = IndicatorWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = TEXT("ClickIndicatorWnd");
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        TEXT("ClickIndicatorWnd"), TEXT(""),
        WS_POPUP,
        0, 0, INDICATOR_SIZE, INDICATOR_SIZE,
        NULL, NULL, hInst, NULL);

    /* 整体透明度 200/255 */
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 200, LWA_ALPHA);

    g_hIndicatorWnd = hwnd;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

/* ========== 连续点击线程 ========== */
DWORD WINAPI click_thread(LPVOID param) {
    (void)param;
    while (!g_state.exit_flag) {
        if (g_state.running) {
            do_click(g_state.mouse_button);
            InterlockedIncrement((LONG*)&g_state.clicked);
            if (g_state.click_count > 0 && g_state.clicked >= g_state.click_count)
                g_state.running = 0;
            Sleep(g_state.interval_ms);
        } else {
            Sleep(10);
        }
    }
    return 0;
}

/* ========== 录制线程 ========== */
DWORD WINAPI record_thread(LPVOID param) {
    (void)param;
    int prev_left = 0, prev_right = 0, prev_middle = 0;
    DWORD last_time = GetTickCount();
    while (!g_state.exit_flag) {
        if (!g_state.recording) {
            prev_left = prev_right = prev_middle = 0;
            Sleep(50);
            continue;
        }
        if (g_state.record_count >= MAX_RECORD_POINTS) {
            g_state.recording = 0;
            continue;
        }
        int left   = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        int right  = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        int middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        int detected = 0;
        DWORD now = GetTickCount();
        int idx = g_state.record_count;
        if (left && !prev_left) {
            GetCursorPos(&g_state.records[idx].pos);
            g_state.records[idx].button = MOUSEEVENTF_LEFTDOWN;
            g_state.records[idx].delay_ms = now - last_time;
            detected = 1;
        } else if (right && !prev_right) {
            GetCursorPos(&g_state.records[idx].pos);
            g_state.records[idx].button = MOUSEEVENTF_RIGHTDOWN;
            g_state.records[idx].delay_ms = now - last_time;
            detected = 1;
        } else if (middle && !prev_middle) {
            GetCursorPos(&g_state.records[idx].pos);
            g_state.records[idx].button = MOUSEEVENTF_MIDDLEDOWN;
            g_state.records[idx].delay_ms = now - last_time;
            detected = 1;
        }
        if (detected) { g_state.record_count++; last_time = now; }
        prev_left = left; prev_right = right; prev_middle = middle;
        Sleep(5);
    }
    return 0;
}

/* ========== 回放线程 ========== */
DWORD WINAPI replay_thread(LPVOID param) {
    (void)param;
    while (!g_state.exit_flag) {
        if (!g_state.replaying || g_state.record_count == 0) { Sleep(50); continue; }
        int i;
        for (i = 0; i < g_state.record_count && g_state.replaying && !g_state.exit_flag; i++) {
            DWORD btn;
            switch (g_state.records[i].button) {
                case MOUSEEVENTF_LEFTDOWN:  btn = 0; break;
                case MOUSEEVENTF_RIGHTDOWN: btn = 1; break;
                default:                     btn = 2; break;
            }
            do_click_at(g_state.records[i].pos.x, g_state.records[i].pos.y, (int)btn);
            Sleep(g_state.records[i].delay_ms);
        }
    }
    return 0;
}

/* ========== UI 绘制 ========== */
static void draw_menu(void) {
    char count_buf[32];
    if (g_state.click_count == 0)
        strcpy(count_buf, "无限");
    else
        sprintf(count_buf, "%d 次", g_state.click_count);

    goto_xy(0, 0);
    printf("============================================\n");
    printf("       连 点 器  v1.1\n");
    printf("============================================\n");
    printf("  [1] 点击速度: %d ms/次  (%.1f 次/秒)\n",
           g_state.interval_ms, 1000.0 / g_state.interval_ms);
    printf("  [2] 点击次数: %s\n", count_buf);
    printf("  [3] 鼠标按键: %s\n",
           g_state.mouse_button == 0 ? "左键" :
           g_state.mouse_button == 1 ? "右键" : "中键");
    printf("  [4] %s\n",
           g_state.running ? "停止点击" : "开始点击");
    printf("  [5] %s\n",
           g_state.replaying ? "停止回放" : "开始回放(录制的点击)");
    printf("  [6] %s\n",
           g_state.recording ? "停止录制" : "开始录制点击");
    printf("  [7] 清除录制数据 (%d 个)\n", g_state.record_count);
    printf("============================================\n");
    printf("  快捷键: F6=开始/停止点击  F7=录制  F8=退出\n");
    printf("============================================");

    clear_line(SEP_LINE);
    goto_xy(0, OP_TITLE_LINE);
    printf("--- 操作区 ----------------------------------");
}

static void draw_status(void) {
    clear_line(STATUS_LINE);
    goto_xy(0, STATUS_LINE);

    if (g_state.running)
        printf("  状态: [点击中]  已点击: %d 次  速度: %dms",
               g_state.clicked, g_state.interval_ms);
    else if (g_state.replaying)
        printf("  状态: [回放中]  循环 %d 个录制点击", g_state.record_count);
    else if (g_state.recording)
        printf("  状态: [录制中]  已录制: %d 个点击", g_state.record_count);
    else
        printf("  状态: [空闲]  按 1-7 菜单操作 或快捷键");
}

/* ========== 主函数 ========== */
int main(void) {
    set_cursor_visible(0);
    SetConsoleTitle(TEXT("连点器"));

    g_state.interval_ms  = 100;
    g_state.click_count   = 0;
    g_state.mouse_button  = 0;
    g_state.exit_flag     = 0;

    /* 启动指示器窗口线程 */
    g_state.hIndicatorThread = CreateThread(NULL, 0, indicator_thread, NULL, 0, NULL);
    Sleep(100); /* 等待窗口创建完成 */

    /* 启动工作线程 */
    g_state.hClickThread  = CreateThread(NULL, 0, click_thread,  NULL, 0, NULL);
    g_state.hRecordThread = CreateThread(NULL, 0, record_thread, NULL, 0, NULL);
    g_state.hReplayThread = CreateThread(NULL, 0, replay_thread, NULL, 0, NULL);

    system("cls");
    draw_menu();
    draw_status();

    while (!g_state.exit_flag) {
        static int f6_prev = 0, f7_prev = 0, f8_prev = 0;
        int f6 = (GetAsyncKeyState(HOTKEY_TOGGLE) & 0x8000) != 0;
        int f7 = (GetAsyncKeyState(HOTKEY_RECORD) & 0x8000) != 0;
        int f8 = (GetAsyncKeyState(HOTKEY_EXIT)   & 0x8000) != 0;

        if (f6 && !f6_prev) {
            if (!g_state.replaying) {
                g_state.running = !g_state.running;
                if (g_state.running) g_state.clicked = 0;
                show_msg(g_state.running ? "已开启连续点击" : "已停止点击");
            }
        }
        if (f7 && !f7_prev) {
            if (!g_state.running) {
                if (g_state.replaying) {
                    g_state.replaying = 0;
                    show_msg("已停止回放");
                } else if (g_state.recording) {
                    g_state.recording = 0;
                    g_state.replaying = 1;
                    show_msg("录制完成，开始回放");
                } else {
                    g_state.record_count = 0;
                    g_state.recording = 1;
                    show_msg("开始录制，点击鼠标即可录入");
                }
            }
        }
        if (f8 && !f8_prev) {
            g_state.exit_flag = 1;
        }
        f6_prev = f6; f7_prev = f7; f8_prev = f8;

        if (_kbhit()) {
            int ch = _getch();
            int val;
            switch (ch) {
                case '1':
                    if (read_int_at(INPUT_LINE, "新间隔(ms, 10-10000):", &val)) {
                        if (val >= 10 && val <= 10000) {
                            g_state.interval_ms = val;
                            show_msg("点击速度已更新");
                        } else {
                            show_msg("无效值，范围 10-10000");
                        }
                    }
                    break;
                case '2':
                    if (read_int_at(INPUT_LINE, "点击次数(0=无限):", &val)) {
                        if (val >= 0) {
                            g_state.click_count = val;
                            show_msg("点击次数已更新");
                        } else {
                            show_msg("次数不能为负数");
                        }
                    }
                    break;
                case '3':
                    if (read_int_at(INPUT_LINE, "按键(0=左键 1=右键 2=中键):", &val)) {
                        if (val >= 0 && val <= 2) {
                            g_state.mouse_button = val;
                            show_msg("鼠标按键已切换");
                        } else {
                            show_msg("无效，请输入 0/1/2");
                        }
                    }
                    break;
                case '4':
                    if (!g_state.replaying) {
                        g_state.running = !g_state.running;
                        if (g_state.running) g_state.clicked = 0;
                        show_msg(g_state.running ? "已开启连续点击" : "已停止点击");
                    } else {
                        show_msg("请先停止回放再操作");
                    }
                    break;
                case '5':
                    if (!g_state.running) {
                        if (g_state.replaying) {
                            g_state.replaying = 0;
                            show_msg("已停止回放");
                        } else if (g_state.record_count > 0) {
                            g_state.replaying = 1;
                            show_msg("开始循环回放录制的点击");
                        } else {
                            show_msg("无录制数据，请先录制");
                        }
                    } else {
                        show_msg("请先停止点击再操作");
                    }
                    break;
                case '6':
                    if (!g_state.running && !g_state.replaying) {
                        if (g_state.recording) {
                            g_state.recording = 0;
                            show_msg("已停止录制");
                        } else {
                            g_state.record_count = 0;
                            g_state.recording = 1;
                            show_msg("开始录制，点击鼠标即可录入");
                        }
                    } else {
                        show_msg("请先停止点击/回放再操作");
                    }
                    break;
                case '7':
                    if (!g_state.running && !g_state.replaying) {
                        g_state.recording = 0;
                        g_state.record_count = 0;
                        show_msg("录制数据已清除");
                    } else {
                        show_msg("请先停止点击/回放再操作");
                    }
                    break;
            }
            draw_menu();
        }

        draw_status();
        Sleep(MENU_REFRESH_MS);
    }

    /* 清理 */
    g_state.exit_flag = 1;
    PostMessage(g_hIndicatorWnd, WM_QUIT, 0, 0);
    WaitForSingleObject(g_state.hClickThread,     2000);
    WaitForSingleObject(g_state.hRecordThread,    2000);
    WaitForSingleObject(g_state.hReplayThread,    2000);
    WaitForSingleObject(g_state.hIndicatorThread, 2000);
    CloseHandle(g_state.hClickThread);
    CloseHandle(g_state.hRecordThread);
    CloseHandle(g_state.hReplayThread);
    CloseHandle(g_state.hIndicatorThread);

    system("cls");
    printf("连点器已退出。按任意键关闭...\n");
    _getch();
    return 0;
}
