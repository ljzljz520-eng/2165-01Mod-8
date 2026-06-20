#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define GRID_W 12
#define GRID_H 8
#define MAX_PATH 64
#define MAX_TOWERS 32
#define MAX_ENEMIES 128
#define BASE_WAVES 5
#define MAX_SUPPORTED_WAVES 30

typedef struct {
    int x;
    int y;
} Point;

typedef struct {
    int x;
    int y;
    int level;
    int range;
    int damage;
    int upgrade_cost;
    bool active;
} Tower;

typedef struct {
    int hp;
    int max_hp;
    int path_idx;
    int reward;
    int move_interval;
    int move_tick;
    bool alive;
} Enemy;

typedef struct {
    int enemy_count;
    int enemy_hp;
    int spawn_interval;
    int move_interval;
    int reward;
} WaveConfig;

typedef struct {
    int hp;
    int gold;
    int wave;
    int max_waves;
    Point path[MAX_PATH];
    int path_len;
    Tower towers[MAX_TOWERS];
    Enemy enemies[MAX_ENEMIES];
} Game;

static const WaveConfig BASE_WAVE_TABLE[BASE_WAVES] = {
    {6, 8, 3, 2, 2},
    {8, 10, 3, 2, 2},
    {10, 12, 2, 2, 3},
    {12, 15, 2, 1, 3},
    {14, 18, 2, 1, 4},
};

static long g_tick_sleep_ms = 180;
static long g_wave_clear_sleep_ms = 700;
static bool g_no_clear = false;

typedef struct {
    struct termios original;
    bool active;
} RawModeGuard;

typedef enum {
    INPUT_ACTION_NONE = 0,
    INPUT_ACTION_LEFT,
    INPUT_ACTION_RIGHT,
    INPUT_ACTION_HOME,
    INPUT_ACTION_END,
    INPUT_ACTION_DELETE,
} InputAction;

static bool read_stdin_byte(unsigned char *out) {
    while (true) {
        ssize_t n = read(STDIN_FILENO, out, 1);
        if (n == 1) return true;
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
}

static bool read_stdin_byte_timeout(unsigned char *out, int timeout_ms) {
    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int rc = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return false;
        return read_stdin_byte(out);
    }
}

static bool enable_raw_mode(RawModeGuard *guard) {
    guard->active = false;
    if (tcgetattr(STDIN_FILENO, &guard->original) != 0) {
        return false;
    }

    struct termios raw = guard->original;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag |= OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return false;
    }
    guard->active = true;
    return true;
}

static void disable_raw_mode(RawModeGuard *guard) {
    if (!guard->active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &guard->original);
    guard->active = false;
}

static size_t utf8_prev_char_start(const char *s, size_t idx) {
    if (idx == 0) return 0;
    idx -= 1;
    while (idx > 0 && (((unsigned char)s[idx] & 0xC0U) == 0x80U)) {
        idx -= 1;
    }
    return idx;
}

static size_t utf8_next_char_start(const char *s, size_t len, size_t idx) {
    if (idx >= len) return len;

    unsigned char lead = (unsigned char)s[idx];
    size_t width = 1;
    if ((lead & 0x80U) == 0) {
        width = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
        width = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
        width = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
        width = 4;
    } else {
        width = 1;
    }

    if (idx + width > len) return idx + 1;
    for (size_t i = 1; i < width; i++) {
        if ((((unsigned char)s[idx + i]) & 0xC0U) != 0x80U) {
            return idx + 1;
        }
    }
    return idx + width;
}

static int utf8_display_width(const char *s, size_t len) {
    mbstate_t st;
    memset(&st, 0, sizeof(st));

    size_t i = 0;
    int width = 0;
    while (i < len) {
        wchar_t wc = 0;
        size_t rc = mbrtowc(&wc, s + i, len - i, &st);
        if (rc == (size_t)-1 || rc == (size_t)-2) {
            memset(&st, 0, sizeof(st));
            i += 1;
            width += 1;
            continue;
        }
        if (rc == 0) {
            i += 1;
            continue;
        }

        int w = wcwidth(wc);
        if (w < 0) w = 1;
        width += w;
        i += rc;
    }
    return width;
}

static void render_input_line(const char *prompt, const char *buf, size_t len, size_t cursor) {
    printf("\r\033[2K%s", prompt);
    if (len > 0) {
        fwrite(buf, 1, len, stdout);
    }

    int full_width = utf8_display_width(buf, len);
    int cursor_width = utf8_display_width(buf, cursor);
    int move_left = full_width - cursor_width;
    if (move_left > 0) {
        printf("\033[%dD", move_left);
    }
    fflush(stdout);
}

static void consume_stdin_until_newline(void) {
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {
    }
}

static bool read_command_line_fallback(const char *prompt, char *out, size_t out_cap) {
    if (out_cap < 2) return false;

    printf("%s", prompt);
    fflush(stdout);
    if (fgets(out, out_cap, stdin) == NULL) {
        return false;
    }

    if (strchr(out, '\n') == NULL) {
        size_t len = strlen(out);
        consume_stdin_until_newline();
        if (len + 1 < out_cap) {
            out[len] = '\n';
            out[len + 1] = '\0';
        }
    }
    return true;
}

static InputAction read_escape_action(void) {
    unsigned char ch = 0;
    if (!read_stdin_byte_timeout(&ch, 35)) {
        return INPUT_ACTION_NONE;
    }

    if (ch == 'O') {
        if (!read_stdin_byte_timeout(&ch, 35)) return INPUT_ACTION_NONE;
        if (ch == 'H') return INPUT_ACTION_HOME;
        if (ch == 'F') return INPUT_ACTION_END;
        return INPUT_ACTION_NONE;
    }

    if (ch != '[') return INPUT_ACTION_NONE;

    char seq[16];
    size_t n = 0;
    while (n < sizeof(seq) - 1) {
        if (!read_stdin_byte_timeout(&ch, 35)) break;
        seq[n++] = (char)ch;
        if (ch >= '@' && ch <= '~') break;
    }
    if (n == 0) return INPUT_ACTION_NONE;
    seq[n] = '\0';

    char final = seq[n - 1];
    if (final == 'D') return INPUT_ACTION_LEFT;
    if (final == 'C') return INPUT_ACTION_RIGHT;
    if (final == 'H') return INPUT_ACTION_HOME;
    if (final == 'F') return INPUT_ACTION_END;
    if (final != '~') return INPUT_ACTION_NONE;

    int code = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)seq[i])) break;
        code = code * 10 + (seq[i] - '0');
    }

    if (code == 1 || code == 7) return INPUT_ACTION_HOME;
    if (code == 4 || code == 8) return INPUT_ACTION_END;
    if (code == 3) return INPUT_ACTION_DELETE;
    return INPUT_ACTION_NONE;
}

static bool read_command_line(const char *prompt, char *out, size_t out_cap) {
    if (out_cap < 2) return false;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return read_command_line_fallback(prompt, out, out_cap);
    }

    RawModeGuard guard;
    if (!enable_raw_mode(&guard)) {
        return read_command_line_fallback(prompt, out, out_cap);
    }

    out[0] = '\0';
    size_t len = 0;
    size_t cursor = 0;
    size_t max_bytes = out_cap - 2;  // reserve '\n' + '\0'

    render_input_line(prompt, out, len, cursor);

    while (true) {
        unsigned char ch = 0;
        if (!read_stdin_byte(&ch)) {
            disable_raw_mode(&guard);
            printf("\r\n");
            fflush(stdout);
            return false;
        }

        bool needs_rerender = false;
        if (ch == '\r' || ch == '\n') {
            out[len] = '\n';
            out[len + 1] = '\0';
            disable_raw_mode(&guard);
            printf("\r\n");
            fflush(stdout);
            return true;
        } else if (ch == 0x03U) {  // Ctrl-C
            disable_raw_mode(&guard);
            printf("\r\n");
            fflush(stdout);
            return false;
        } else if (ch == 0x04U) {  // Ctrl-D
            if (len == 0) {
                disable_raw_mode(&guard);
                printf("\r\n");
                fflush(stdout);
                return false;
            }
        } else if (ch == 0x7FU || ch == 0x08U) {
            if (cursor > 0) {
                size_t start = utf8_prev_char_start(out, cursor);
                memmove(out + start, out + cursor, len - cursor);
                len -= (cursor - start);
                cursor = start;
                out[len] = '\0';
                needs_rerender = true;
            }
        } else if (ch == 0x1BU) {
            InputAction action = read_escape_action();
            if (action == INPUT_ACTION_LEFT && cursor > 0) {
                cursor = utf8_prev_char_start(out, cursor);
                needs_rerender = true;
            } else if (action == INPUT_ACTION_RIGHT && cursor < len) {
                cursor = utf8_next_char_start(out, len, cursor);
                needs_rerender = true;
            } else if (action == INPUT_ACTION_HOME) {
                cursor = 0;
                needs_rerender = true;
            } else if (action == INPUT_ACTION_END) {
                cursor = len;
                needs_rerender = true;
            } else if (action == INPUT_ACTION_DELETE && cursor < len) {
                size_t next = utf8_next_char_start(out, len, cursor);
                memmove(out + cursor, out + next, len - next);
                len -= (next - cursor);
                out[len] = '\0';
                needs_rerender = true;
            }
        } else if (ch == 0x01U) {  // Ctrl-A
            cursor = 0;
            needs_rerender = true;
        } else if (ch == 0x05U) {  // Ctrl-E
            cursor = len;
            needs_rerender = true;
        } else if (ch >= 0x20U && ch != 0x7FU) {
            if (len < max_bytes) {
                memmove(out + cursor + 1, out + cursor, len - cursor);
                out[cursor] = (char)ch;
                len += 1;
                cursor += 1;
                out[len] = '\0';
                needs_rerender = true;
            }
        }

        if (needs_rerender) {
            render_input_line(prompt, out, len, cursor);
        }
    }
}

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static long read_sleep_ms_env(const char *name, long default_value, long max_value) {
    const char *raw = getenv(name);
    if (raw == NULL || raw[0] == '\0') {
        return default_value;
    }

    char *end = NULL;
    long parsed = strtol(raw, &end, 10);
    if (end == raw || *end != '\0') {
        return default_value;
    }

    if (parsed < 0) parsed = 0;
    if (parsed > max_value) parsed = max_value;
    return parsed;
}

static int read_max_waves_from_env(void) {
    const char *raw = getenv("TD_MAX_WAVES");
    if (raw == NULL || raw[0] == '\0') {
        return BASE_WAVES;
    }

    char *end = NULL;
    long parsed = strtol(raw, &end, 10);
    if (end == raw || *end != '\0') {
        return BASE_WAVES;
    }

    if (parsed < 1) parsed = 1;
    if (parsed > MAX_SUPPORTED_WAVES) parsed = MAX_SUPPORTED_WAVES;
    return (int)parsed;
}

static WaveConfig get_wave_config(int wave) {
    if (wave <= BASE_WAVES) {
        return BASE_WAVE_TABLE[wave - 1];
    }

    WaveConfig cfg = BASE_WAVE_TABLE[BASE_WAVES - 1];
    int extra = wave - BASE_WAVES;
    cfg.enemy_count += extra;
    cfg.enemy_hp += extra * 2;
    cfg.spawn_interval -= extra / 3;
    if (cfg.spawn_interval < 1) cfg.spawn_interval = 1;
    cfg.reward += extra / 2;
    if (cfg.reward > 9) cfg.reward = 9;
    return cfg;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void init_path(Game *game) {
    Point route[] = {
        {0, 3}, {1, 3}, {2, 3}, {3, 3},
        {4, 3}, {4, 4}, {4, 5}, {5, 5},
        {6, 5}, {7, 5}, {8, 5}, {9, 5},
        {10, 5}, {11, 5}
    };
    game->path_len = (int)(sizeof(route) / sizeof(route[0]));
    for (int i = 0; i < game->path_len; i++) {
        game->path[i] = route[i];
    }
}

static bool is_path_cell(const Game *game, int x, int y) {
    for (int i = 0; i < game->path_len; i++) {
        if (game->path[i].x == x && game->path[i].y == y) {
            return true;
        }
    }
    return false;
}

static Tower *find_tower(Game *game, int x, int y) {
    for (int i = 0; i < MAX_TOWERS; i++) {
        if (game->towers[i].active && game->towers[i].x == x && game->towers[i].y == y) {
            return &game->towers[i];
        }
    }
    return NULL;
}

static int tower_count(const Game *game) {
    int count = 0;
    for (int i = 0; i < MAX_TOWERS; i++) {
        if (game->towers[i].active) {
            count++;
        }
    }
    return count;
}

static void init_game(Game *game) {
    memset(game, 0, sizeof(*game));
    game->hp = 10;
    game->gold = 20;
    game->wave = 1;
    game->max_waves = read_max_waves_from_env();
    g_tick_sleep_ms = read_sleep_ms_env("TD_TICK_MS", 180, 5000);
    g_wave_clear_sleep_ms = read_sleep_ms_env("TD_WAVE_CLEAR_MS", 700, 10000);
    g_no_clear = getenv("TD_NO_CLEAR") != NULL;
    init_path(game);
}

static void clear_screen(void) {
    if (g_no_clear) {
        return;
    }
    printf("\033[2J\033[H");
}

static void render_board(const Game *game, bool show_enemy_hp) {
    char grid[GRID_H][GRID_W];
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            grid[y][x] = '.';
        }
    }

    for (int i = 0; i < game->path_len; i++) {
        int x = game->path[i].x;
        int y = game->path[i].y;
        grid[y][x] = '#';
    }

    for (int i = 0; i < MAX_TOWERS; i++) {
        if (!game->towers[i].active) continue;
        int x = game->towers[i].x;
        int y = game->towers[i].y;
        grid[y][x] = (game->towers[i].level >= 3) ? 'A' : 'T';
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!game->enemies[i].alive) continue;
        Point pos = game->path[clamp_int(game->enemies[i].path_idx, 0, game->path_len - 1)];
        grid[pos.y][pos.x] = 'E';
    }

    printf("=== 明日方舟迷你塔防（C 版）===\n");
    printf("波次: %d/%d | 基地生命: %d | 费用: %d | 干员数: %d\n",
           game->wave, game->max_waves, game->hp, game->gold, tower_count(game));
    printf("图例: # 路径, T 干员, A 高级干员, E 敌人\n\n");

    printf("   ");
    for (int x = 0; x < GRID_W; x++) {
        printf("%d ", x);
    }
    printf("\n");

    for (int y = 0; y < GRID_H; y++) {
        printf("%d: ", y);
        for (int x = 0; x < GRID_W; x++) {
            printf("%c ", grid[y][x]);
        }
        printf("\n");
    }

    if (!show_enemy_hp) return;

    printf("\n在场敌人: ");
    bool any = false;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!game->enemies[i].alive) continue;
        printf("[生命:%d 位置:%d] ", game->enemies[i].hp, game->enemies[i].path_idx);
        any = true;
    }
    if (!any) {
        printf("无");
    }
    printf("\n");
}

static int distance_sq(int x1, int y1, int x2, int y2) {
    int dx = x1 - x2;
    int dy = y1 - y2;
    return dx * dx + dy * dy;
}

static bool add_tower(Game *game, int x, int y) {
    const int cost = 8;
    if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) {
        printf("坐标超出地图范围。\n");
        return false;
    }
    if (is_path_cell(game, x, y)) {
        printf("该位置是敌人路径，不能部署。\n");
        return false;
    }
    if (find_tower(game, x, y) != NULL) {
        printf("该位置已有干员。\n");
        return false;
    }
    if (game->gold < cost) {
        printf("费用不足，部署需要 %d。\n", cost);
        return false;
    }

    for (int i = 0; i < MAX_TOWERS; i++) {
        if (game->towers[i].active) continue;
        game->towers[i].active = true;
        game->towers[i].x = x;
        game->towers[i].y = y;
        game->towers[i].level = 1;
        game->towers[i].range = 2;
        game->towers[i].damage = 3;
        game->towers[i].upgrade_cost = 10;
        game->gold -= cost;
        printf("部署成功：(%d,%d)\n", x, y);
        return true;
    }

    printf("部署上限已满。\n");
    return false;
}

static bool upgrade_tower(Game *game, int x, int y) {
    Tower *tower = find_tower(game, x, y);
    if (tower == NULL) {
        printf("该位置没有可升级的干员。\n");
        return false;
    }
    if (tower->level >= 3) {
        printf("已达到最高等级。\n");
        return false;
    }
    if (game->gold < tower->upgrade_cost) {
        printf("费用不足，升级需要 %d。\n", tower->upgrade_cost);
        return false;
    }
    game->gold -= tower->upgrade_cost;
    tower->level += 1;
    tower->range += 1;
    tower->damage += 2;
    tower->upgrade_cost += 6;
    printf("升级成功：(%d,%d) -> Lv.%d\n", x, y, tower->level);
    return true;
}

static bool spawn_enemy(Game *game, const WaveConfig *cfg) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (game->enemies[i].alive) continue;
        game->enemies[i].alive = true;
        game->enemies[i].max_hp = cfg->enemy_hp;
        game->enemies[i].hp = cfg->enemy_hp;
        game->enemies[i].path_idx = 0;
        game->enemies[i].reward = cfg->reward;
        game->enemies[i].move_interval = cfg->move_interval;
        game->enemies[i].move_tick = 0;
        return true;
    }
    return false;
}

static void towers_attack(Game *game) {
    for (int i = 0; i < MAX_TOWERS; i++) {
        Tower *tower = &game->towers[i];
        if (!tower->active) continue;

        Enemy *target = NULL;
        int best_path_progress = -1;
        int range_sq = tower->range * tower->range;
        for (int j = 0; j < MAX_ENEMIES; j++) {
            Enemy *enemy = &game->enemies[j];
            if (!enemy->alive) continue;
            Point pos = game->path[enemy->path_idx];
            if (distance_sq(tower->x, tower->y, pos.x, pos.y) <= range_sq) {
                if (enemy->path_idx > best_path_progress) {
                    best_path_progress = enemy->path_idx;
                    target = enemy;
                }
            }
        }

        if (target == NULL) continue;
        target->hp -= tower->damage;
        if (target->hp <= 0) {
            target->alive = false;
            game->gold += target->reward;
        }
    }
}

static void enemies_move(Game *game) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *enemy = &game->enemies[i];
        if (!enemy->alive) continue;

        enemy->move_tick += 1;
        if (enemy->move_tick < enemy->move_interval) continue;
        enemy->move_tick = 0;
        enemy->path_idx += 1;

        if (enemy->path_idx >= game->path_len) {
            enemy->alive = false;
            game->hp -= 1;
        }
    }
}

static bool has_live_enemies(const Game *game) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (game->enemies[i].alive) return true;
    }
    return false;
}

static void print_tips(void) {
    printf("\n新手三步上手：\n");
    printf("  1) 输入 地图 查看路径(#)与坐标，干员不能放在 # 上。\n");
    printf("  2) 输入 部署 x y 放置干员（例如：部署 3 2）。\n");
    printf("  3) 输入 开始 进入战斗。\n");
    printf("\n进阶提示：\n");
    printf("  - 敌人走到终点会让基地生命 -1。\n");
    printf("  - 击杀敌人会获得费用，波次结束额外 +5 费用。\n");
    printf("  - 费用够时可用 升级 x y 强化已有干员。\n");
    printf("  - 战斗阶段是自动进行，战斗中不能输入指令。\n");
    printf("  - 随时输入 help/帮助 或 tips/提示 查看说明。\n\n");
}

static void print_phase_hint(const Game *game) {
    int towers = tower_count(game);
    if (towers == 0) {
        printf("提示：你还没有部署干员。建议先试试：部署 3 2 或 部署 5 4\n");
        return;
    }
    if (game->gold >= 8) {
        printf("提示：当前费用 %d，可继续部署（部署 x y）或升级（升级 x y）。\n", game->gold);
    } else {
        printf("提示：当前费用 %d，建议先开始战斗赚取费用。\n", game->gold);
    }
    if (game->hp <= 3) {
        printf("警告：基地生命仅剩 %d，建议优先升级前线干员。\n", game->hp);
    }
}

static void print_help(void) {
    printf("\n可用指令：\n");
    printf("  place x y / 部署 x y      在坐标部署干员 (费用 8)\n");
    printf("  upgrade x y / 升级 x y    升级干员，最多 Lv.3\n");
    printf("  start / 开始              开始当前波次\n");
    printf("  map / 地图                显示地图\n");
    printf("  help / 帮助               显示帮助\n");
    printf("  tips / 提示               查看玩法提示\n");
    printf("  quit / 退出               退出游戏\n\n");
    printf("说明：战斗阶段为自动战斗，无法输入指令；请在准备阶段完成部署和升级。\n\n");
}

static bool starts_with_icase(const char *line, const char *prefix) {
    size_t n = strlen(prefix);
    return strncasecmp(line, prefix, n) == 0;
}

static bool prep_phase(Game *game) {
    char line[128];
    char prompt[32];
    bool confirm_empty_start = false;
    print_help();
    print_phase_hint(game);
    while (true) {
        snprintf(prompt, sizeof(prompt), "第%d波 > ", game->wave);
        if (!read_command_line(prompt, line, sizeof(line))) {
            return false;
        }

        int x = -1;
        int y = -1;
        bool place_en = starts_with_icase(line, "place");
        bool place_cn = strncmp(line, "部署", strlen("部署")) == 0;
        bool upgrade_en = starts_with_icase(line, "upgrade");
        bool upgrade_cn = strncmp(line, "升级", strlen("升级")) == 0;

        if (place_en || place_cn) {
            if ((place_en && sscanf(line, "%*s %d %d", &x, &y) == 2) ||
                (place_cn && sscanf(line, "部署 %d %d", &x, &y) == 2)) {
                add_tower(game, x, y);
            } else {
                printf("格式错误：place x y 或 部署 x y\n");
            }
            confirm_empty_start = false;
        } else if (upgrade_en || upgrade_cn) {
            if ((upgrade_en && sscanf(line, "%*s %d %d", &x, &y) == 2) ||
                (upgrade_cn && sscanf(line, "升级 %d %d", &x, &y) == 2)) {
                upgrade_tower(game, x, y);
            } else {
                printf("格式错误：upgrade x y 或 升级 x y\n");
            }
            confirm_empty_start = false;
        } else if (starts_with_icase(line, "map") || strncmp(line, "地图", strlen("地图")) == 0) {
            render_board(game, false);
            print_phase_hint(game);
            confirm_empty_start = false;
        } else if (starts_with_icase(line, "help") || strncmp(line, "帮助", strlen("帮助")) == 0) {
            print_help();
            confirm_empty_start = false;
        } else if (starts_with_icase(line, "tips") || strncmp(line, "提示", strlen("提示")) == 0) {
            print_tips();
            confirm_empty_start = false;
        } else if (starts_with_icase(line, "start") || strncmp(line, "开始", strlen("开始")) == 0) {
            if (tower_count(game) == 0 && !confirm_empty_start) {
                printf("当前没有已部署干员，开战后很容易掉基地生命。\n");
                printf("建议先输入：部署 3 2（或其他非路径坐标）。\n");
                printf("若仍要开战，请再次输入 start / 开始 确认。\n");
                confirm_empty_start = true;
                continue;
            }
            return true;
        } else if (starts_with_icase(line, "quit") || strncmp(line, "退出", strlen("退出")) == 0) {
            return false;
        } else if (line[0] == '\n' || line[0] == '\0') {
            continue;
        } else {
            printf("未知指令，输入 help 或 帮助 查看说明。\n");
            confirm_empty_start = false;
        }
    }
}

static bool run_wave(Game *game, const WaveConfig *cfg) {
    int spawned = 0;
    int tick = 0;
    while (true) {
        if (spawned < cfg->enemy_count && tick % cfg->spawn_interval == 0) {
            if (spawn_enemy(game, cfg)) {
                spawned += 1;
            }
        }

        towers_attack(game);
        enemies_move(game);

        clear_screen();
        render_board(game, true);
        printf("第 %d 波 | 战斗轮次: %d\n", game->wave, tick);
        printf("战斗提示：自动战斗中，敌人到达终点会扣基地生命，击杀敌人可获得费用。\n");

        if (game->hp <= 0) {
            return false;
        }

        if (spawned >= cfg->enemy_count && !has_live_enemies(game)) {
            game->gold += 5;
            printf("波次 %d 完成！额外获得 5 费用。\n", game->wave);
            sleep_ms(g_wave_clear_sleep_ms);
            return true;
        }

        tick += 1;
        sleep_ms(g_tick_sleep_ms);
    }
}

int main(void) {
    setlocale(LC_ALL, "");

    Game game;
    init_game(&game);

    clear_screen();
    printf("欢迎来到简化版明日方舟塔防（C 终端版）\n");
    printf("目标：守住 %d 波敌人，基地血量归零则失败。\n\n", game.max_waves);
    print_tips();

    while (game.wave <= game.max_waves) {
        render_board(&game, false);
        printf("\n准备阶段：部署/升级后输入 start 或 开始 进入战斗。\n");
        if (!prep_phase(&game)) {
            printf("\n游戏结束（主动退出）。\n");
            return 0;
        }

        WaveConfig cfg = get_wave_config(game.wave);
        if (!run_wave(&game, &cfg)) {
            clear_screen();
            render_board(&game, false);
            printf("\n基地失守，游戏失败。\n");
            return 0;
        }

        if (game.wave == game.max_waves) {
            break;
        }
        game.wave += 1;
    }

    clear_screen();
    render_board(&game, false);
    printf("\n恭喜通关！你成功守住了所有波次。\n");
    return 0;
}
