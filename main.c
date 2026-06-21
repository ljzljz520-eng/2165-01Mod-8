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
#define MAX_PATHS 2
#define MAX_ENEMY_TYPES 4
#define MAX_LEAKS 512

typedef enum {
    ENEMY_SCOUT = 0,
    ENEMY_SOLDIER,
    ENEMY_BULK,
    ENEMY_BOSS
} EnemyType;

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
    EnemyType type;
    int route_id;
    int spawn_index;
} Enemy;

typedef struct {
    EnemyType type;
    int count;
    int hp;
    int spawn_interval;
    int move_interval;
    int reward;
    int route_id;
} WaveGroup;

typedef struct {
    int group_count;
    WaveGroup groups[MAX_ENEMY_TYPES * MAX_PATHS];
} WaveConfig;

typedef struct {
    int wave;
    int route_id;
    EnemyType type;
    int count;
    int hp_lost;
} LeakRecord;

typedef struct {
    Point path[MAX_PATH];
    int path_len;
} Route;

typedef struct {
    int hp;
    int initial_hp;
    int gold;
    int wave;
    int max_waves;
    Route routes[MAX_PATHS];
    int route_count;
    Tower towers[MAX_TOWERS];
    Enemy enemies[MAX_ENEMIES];
    LeakRecord leaks[MAX_LEAKS];
    int leak_count;
    int total_hp_lost;
    int total_leaked_enemies;
    bool hard_mode;
} Game;

static const char *ENEMY_TYPE_NAMES[MAX_ENEMY_TYPES] = {
    "侦察兵(SCOUT)",
    "士兵(SOLDIER)",
    "重装(BULK)",
    "首领(BOSS)"
};

static const char *ENEMY_TYPE_SHORT[MAX_ENEMY_TYPES] = {
    "Sc", "So", "Bk", "Bo"
};

static const int ENEMY_HP_COST[MAX_ENEMY_TYPES] = { 1, 1, 2, 5 };
static const char *ENEMY_HP_COST_DESC[MAX_ENEMY_TYPES] = {
    "基地 -1", "基地 -1", "基地 -2", "基地 -5"
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
    size_t max_bytes = out_cap - 2;

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
        } else if (ch == 0x03U) {
            disable_raw_mode(&guard);
            printf("\r\n");
            fflush(stdout);
            return false;
        } else if (ch == 0x04U) {
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
        } else if (ch == 0x01U) {
            cursor = 0;
            needs_rerender = true;
        } else if (ch == 0x05U) {
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

static void add_group(WaveConfig *cfg, EnemyType type, int count, int hp,
                      int spawn_interval, int move_interval, int reward, int route_id) {
    if (cfg->group_count >= MAX_ENEMY_TYPES * MAX_PATHS) return;
    WaveGroup *g = &cfg->groups[cfg->group_count++];
    g->type = type;
    g->count = count;
    g->hp = hp;
    g->spawn_interval = spawn_interval;
    g->move_interval = move_interval;
    g->reward = reward;
    g->route_id = route_id;
}

static WaveConfig get_wave_config(int wave) {
    WaveConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (wave == 1) {
        add_group(&cfg, ENEMY_SCOUT, 6, 8, 3, 2, 2, 0);
    } else if (wave == 2) {
        add_group(&cfg, ENEMY_SCOUT, 4, 10, 3, 2, 2, 0);
        add_group(&cfg, ENEMY_SOLDIER, 4, 10, 3, 2, 3, 0);
    } else if (wave == 3) {
        add_group(&cfg, ENEMY_SOLDIER, 8, 12, 2, 2, 3, 0);
        add_group(&cfg, ENEMY_SCOUT, 4, 10, 3, 2, 2, 1);
    } else if (wave == 4) {
        add_group(&cfg, ENEMY_SOLDIER, 8, 15, 2, 2, 3, 0);
        add_group(&cfg, ENEMY_BULK, 2, 30, 3, 3, 5, 0);
        add_group(&cfg, ENEMY_SCOUT, 4, 12, 2, 2, 2, 1);
    } else if (wave == 5) {
        add_group(&cfg, ENEMY_SOLDIER, 10, 18, 2, 2, 3, 0);
        add_group(&cfg, ENEMY_BULK, 3, 40, 3, 3, 6, 0);
        add_group(&cfg, ENEMY_SOLDIER, 4, 15, 2, 2, 3, 1);
    } else if (wave == 6) {
        add_group(&cfg, ENEMY_SOLDIER, 10, 22, 2, 2, 4, 0);
        add_group(&cfg, ENEMY_BULK, 3, 45, 3, 3, 6, 0);
        add_group(&cfg, ENEMY_SCOUT, 6, 14, 2, 2, 3, 1);
    } else if (wave == 7) {
        add_group(&cfg, ENEMY_SOLDIER, 12, 25, 2, 2, 4, 0);
        add_group(&cfg, ENEMY_BULK, 4, 50, 3, 4, 7, 0);
        add_group(&cfg, ENEMY_BOSS, 1, 50, 6, 4, 15, 0);
        add_group(&cfg, ENEMY_SOLDIER, 4, 20, 3, 2, 4, 1);
    } else if (wave == 8) {
        add_group(&cfg, ENEMY_SOLDIER, 12, 28, 2, 2, 4, 0);
        add_group(&cfg, ENEMY_BULK, 5, 60, 3, 4, 7, 0);
        add_group(&cfg, ENEMY_SOLDIER, 5, 22, 3, 2, 4, 1);
    } else if (wave == 9) {
        add_group(&cfg, ENEMY_SOLDIER, 12, 30, 2, 2, 5, 0);
        add_group(&cfg, ENEMY_BULK, 5, 70, 3, 4, 8, 0);
        add_group(&cfg, ENEMY_BOSS, 1, 70, 6, 4, 18, 0);
        add_group(&cfg, ENEMY_SOLDIER, 5, 25, 3, 2, 5, 1);
    } else if (wave == 10) {
        add_group(&cfg, ENEMY_SOLDIER, 12, 34, 2, 2, 5, 0);
        add_group(&cfg, ENEMY_BULK, 6, 78, 3, 4, 8, 0);
        add_group(&cfg, ENEMY_BOSS, 1, 90, 6, 4, 20, 0);
        add_group(&cfg, ENEMY_SOLDIER, 5, 28, 3, 2, 5, 1);
    } else {
        int extra = wave - 10;
        add_group(&cfg, ENEMY_SOLDIER, 14 + extra, 38 + extra * 5, 2, 2, 5 + extra / 3, 0);
        add_group(&cfg, ENEMY_BULK, 8 + extra / 2, 90 + extra * 10, 3, 3, 8 + extra / 2, 0);
        if (wave % 3 == 0) {
            add_group(&cfg, ENEMY_BOSS, 1 + wave / 10, 120 + extra * 20, 5, 3, 20 + extra, 0);
        }
        add_group(&cfg, ENEMY_SOLDIER, 6 + extra / 2, 32 + extra * 5, 2, 2, 5 + extra / 3, 1);
    }
    return cfg;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void init_routes(Game *game) {
    game->route_count = 2;

    Point route0[] = {
        {0, 3}, {1, 3}, {2, 3}, {3, 3},
        {4, 3}, {4, 4}, {4, 5}, {5, 5},
        {6, 5}, {7, 5}, {8, 5}, {9, 5},
        {10, 5}, {11, 5}
    };
    game->routes[0].path_len = (int)(sizeof(route0) / sizeof(route0[0]));
    for (int i = 0; i < game->routes[0].path_len; i++) {
        game->routes[0].path[i] = route0[i];
    }

    Point route1[] = {
        {0, 0}, {1, 0}, {2, 0}, {3, 0},
        {3, 1}, {3, 2}, {4, 2}, {5, 2},
        {6, 2}, {7, 2}, {8, 2}, {8, 3},
        {8, 4}, {8, 5}, {9, 5}, {10, 5}, {11, 5}
    };
    game->routes[1].path_len = (int)(sizeof(route1) / sizeof(route1[0]));
    for (int i = 0; i < game->routes[1].path_len; i++) {
        game->routes[1].path[i] = route1[i];
    }
}

static bool is_path_cell(const Game *game, int x, int y) {
    for (int r = 0; r < game->route_count; r++) {
        for (int i = 0; i < game->routes[r].path_len; i++) {
            if (game->routes[r].path[i].x == x && game->routes[r].path[i].y == y) {
                return true;
            }
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
        if (game->towers[i].active) count++;
    }
    return count;
}

static void init_game(Game *game) {
    memset(game, 0, sizeof(*game));
    game->initial_hp = 10;
    game->hp = game->initial_hp;
    game->gold = 20;
    game->wave = 1;
    game->max_waves = read_max_waves_from_env();
    g_tick_sleep_ms = read_sleep_ms_env("TD_TICK_MS", 180, 5000);
    g_wave_clear_sleep_ms = read_sleep_ms_env("TD_WAVE_CLEAR_MS", 700, 10000);
    g_no_clear = getenv("TD_NO_CLEAR") != NULL;
    game->hard_mode = getenv("TD_HARD") != NULL;
    if (game->hard_mode) {
        game->initial_hp = 5;
        game->hp = 5;
    }
    init_routes(game);
}

static void clear_screen(void) {
    if (g_no_clear) return;
    printf("\033[2J\033[H");
}

static void render_board(const Game *game, bool show_enemy_hp) {
    char grid[GRID_H][GRID_W];
    int grid_route[GRID_H][GRID_W];
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            grid[y][x] = '.';
            grid_route[y][x] = -1;
        }
    }

    for (int r = 0; r < game->route_count; r++) {
        for (int i = 0; i < game->routes[r].path_len; i++) {
            int x = game->routes[r].path[i].x;
            int y = game->routes[r].path[i].y;
            if (r == 0) {
                grid[y][x] = '#';
            } else {
                grid[y][x] = (grid[y][x] == '#') ? '+' : '%';
            }
            grid_route[y][x] = r;
        }
    }

    for (int i = 0; i < MAX_TOWERS; i++) {
        if (!game->towers[i].active) continue;
        int x = game->towers[i].x;
        int y = game->towers[i].y;
        grid[y][x] = (game->towers[i].level >= 3) ? 'A' : 'T';
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!game->enemies[i].alive) continue;
        const Route *rt = &game->routes[clamp_int(game->enemies[i].route_id, 0, game->route_count - 1)];
        Point pos = rt->path[clamp_int(game->enemies[i].path_idx, 0, rt->path_len - 1)];
        switch (game->enemies[i].type) {
            case ENEMY_SCOUT:    grid[pos.y][pos.x] = 's'; break;
            case ENEMY_SOLDIER:  grid[pos.y][pos.x] = 'E'; break;
            case ENEMY_BULK:     grid[pos.y][pos.x] = 'B'; break;
            case ENEMY_BOSS:     grid[pos.y][pos.x] = 'X'; break;
            default:             grid[pos.y][pos.x] = 'E'; break;
        }
    }

    printf("=== 明日方舟迷你塔防（C 版）===\n");
    printf("波次: %d/%d | 基地生命: %d/%d | 费用: %d | 干员数: %d | 路线: %d\n",
           game->wave, game->max_waves, game->hp, game->initial_hp,
           game->gold, tower_count(game), game->route_count);
    if (game->hard_mode) {
        printf("[困难模式] ");
    }
    printf("图例: # 路线A, %% 路线B, + 重叠, T 干员, A 高级干员\n");
    printf("敌人: s 侦察(基地-1) E 士兵(基地-1) B 重装(基地-2) X 首领(基地-5)\n\n");

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
        int route_id = game->enemies[i].route_id;
        printf("[%s 路线%c 生命:%d/%d 位置:%d] ",
               ENEMY_TYPE_SHORT[game->enemies[i].type],
               'A' + route_id,
               game->enemies[i].hp,
               game->enemies[i].max_hp,
               game->enemies[i].path_idx);
        any = true;
    }
    if (!any) printf("无");
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

static bool spawn_enemy(Game *game, const WaveGroup *group, int spawn_index) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (game->enemies[i].alive) continue;
        game->enemies[i].alive = true;
        game->enemies[i].type = group->type;
        game->enemies[i].max_hp = group->hp;
        game->enemies[i].hp = group->hp;
        game->enemies[i].path_idx = 0;
        game->enemies[i].reward = group->reward;
        game->enemies[i].move_interval = group->move_interval;
        game->enemies[i].move_tick = 0;
        game->enemies[i].route_id = group->route_id;
        game->enemies[i].spawn_index = spawn_index;
        return true;
    }
    return false;
}

static void record_leak(Game *game, int wave, int route_id, EnemyType type) {
    if (game->leak_count >= MAX_LEAKS) return;
    for (int i = 0; i < game->leak_count; i++) {
        if (game->leaks[i].wave == wave &&
            game->leaks[i].route_id == route_id &&
            game->leaks[i].type == type) {
            game->leaks[i].count += 1;
            game->leaks[i].hp_lost += ENEMY_HP_COST[type];
            game->total_hp_lost += ENEMY_HP_COST[type];
            game->total_leaked_enemies += 1;
            return;
        }
    }
    LeakRecord *rec = &game->leaks[game->leak_count++];
    rec->wave = wave;
    rec->route_id = route_id;
    rec->type = type;
    rec->count = 1;
    rec->hp_lost = ENEMY_HP_COST[type];
    game->total_hp_lost += ENEMY_HP_COST[type];
    game->total_leaked_enemies += 1;
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
            const Route *rt = &game->routes[clamp_int(enemy->route_id, 0, game->route_count - 1)];
            Point pos = rt->path[enemy->path_idx];
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

static void enemies_move(Game *game, int current_wave) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *enemy = &game->enemies[i];
        if (!enemy->alive) continue;

        enemy->move_tick += 1;
        if (enemy->move_tick < enemy->move_interval) continue;
        enemy->move_tick = 0;
        enemy->path_idx += 1;

        const Route *rt = &game->routes[clamp_int(enemy->route_id, 0, game->route_count - 1)];
        if (enemy->path_idx >= rt->path_len) {
            enemy->alive = false;
            int cost = ENEMY_HP_COST[enemy->type];
            game->hp -= cost;
            record_leak(game, current_wave, enemy->route_id, enemy->type);
        }
    }
}

static bool has_live_enemies(const Game *game) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (game->enemies[i].alive) return true;
    }
    return false;
}

static void print_enemy_type_info(void) {
    printf("敌人类型一览：\n");
    for (int t = 0; t < MAX_ENEMY_TYPES; t++) {
        printf("  %d. %-20s 扣血: %s\n",
               t + 1, ENEMY_TYPE_NAMES[t], ENEMY_HP_COST_DESC[t]);
    }
    printf("\n");
}

static void print_tips(void) {
    printf("\n新手三步上手：\n");
    printf("  1) 输入 地图 查看路径(#=路线A, %%=路线B)与坐标，干员不能放在路径上。\n");
    printf("  2) 输入 部署 x y 放置干员（例如：部署 3 2）。\n");
    printf("  3) 输入 开始 进入战斗。\n");
    printf("\n进阶提示：\n");
    printf("  - 不同敌人漏怪扣血不同：%s,%s,%s,%s。\n",
           ENEMY_HP_COST_DESC[0], ENEMY_HP_COST_DESC[1],
           ENEMY_HP_COST_DESC[2], ENEMY_HP_COST_DESC[3]);
    printf("  - 击杀敌人会获得费用，波次结束额外 +5 费用。\n");
    printf("  - 费用够时可用 升级 x y 强化已有干员。\n");
    printf("  - 战斗阶段是自动进行，战斗中不能输入指令。\n");
    printf("  - 战斗结束后会显示漏怪复盘，可输入 配置 调整关卡。\n");
    printf("  - 随时输入 help/帮助 或 tips/提示 查看说明。\n\n");
}

static void print_phase_hint(const Game *game) {
    int towers = tower_count(game);
    if (towers == 0) {
        printf("提示：你还没有部署干员。建议先试试：部署 3 2 或 部署 5 4\n");
        printf("  路线A(左→下→右): (0,3)→(4,5)→(11,5)\n");
        printf("  路线B(左上→右→下): (0,0)→(8,2)→(11,5)，注意交叉点\n");
        return;
    }
    if (game->gold >= 8) {
        printf("提示：当前费用 %d，可继续部署（部署 x y）或升级（升级 x y）。\n", game->gold);
    } else {
        printf("提示：当前费用 %d，建议先开始战斗赚取费用。\n", game->gold);
    }
    if (game->hp <= 5) {
        printf("警告：基地生命仅剩 %d/%d，建议优先升级前线干员。\n", game->hp, game->initial_hp);
    }
}

static void print_help(void) {
    printf("\n可用指令：\n");
    printf("  place x y / 部署 x y      在坐标部署干员 (费用 8)\n");
    printf("  upgrade x y / 升级 x y    升级干员，最多 Lv.3\n");
    printf("  start / 开始              开始当前波次\n");
    printf("  map / 地图                显示地图\n");
    printf("  types / 敌人              显示敌人类型及扣血规则\n");
    printf("  help / 帮助               显示帮助\n");
    printf("  tips / 提示               查看玩法提示\n");
    printf("  quit / 退出               退出游戏\n\n");
    printf("关卡配置调整指令（战斗结束后可用）：\n");
    printf("  config / 配置             进入关卡配置调整模式\n");
    printf("说明：战斗阶段为自动战斗，无法输入指令；请在准备阶段完成部署和升级。\n\n");
}

static bool starts_with_icase(const char *line, const char *prefix) {
    size_t n = strlen(prefix);
    return strncasecmp(line, prefix, n) == 0;
}

static void print_leak_report(const Game *game) {
    printf("\n═══════════════════════ 漏怪复盘报告 ═══════════════════════\n");
    printf("总基地血量损失: %d / %d (%.0f%%)\n",
           game->total_hp_lost, game->initial_hp,
           game->initial_hp > 0 ? 100.0 * game->total_hp_lost / game->initial_hp : 0);
    printf("总漏怪数量: %d 只\n", game->total_leaked_enemies);

    if (game->leak_count == 0) {
        printf("结果：零漏怪！干员们完美守住了所有波次。\n");
        printf("═══════════════════════════════════════════════════════════\n\n");
        return;
    }

    printf("\n── 按波次汇总 ──\n");
    int wave_stats[MAX_SUPPORTED_WAVES + 1] = {0};
    int wave_hp_stats[MAX_SUPPORTED_WAVES + 1] = {0};
    for (int i = 0; i < game->leak_count; i++) {
        wave_stats[game->leaks[i].wave] += game->leaks[i].count;
        wave_hp_stats[game->leaks[i].wave] += game->leaks[i].hp_lost;
    }
    for (int w = 1; w <= game->max_waves; w++) {
        if (wave_stats[w] > 0) {
            printf("  第 %d 波: 漏 %d 只, 失血 %d\n", w, wave_stats[w], wave_hp_stats[w]);
        }
    }

    printf("\n── 按路线汇总 ──\n");
    int route_count_stats[MAX_PATHS] = {0};
    int route_hp_stats[MAX_PATHS] = {0};
    for (int i = 0; i < game->leak_count; i++) {
        int r = clamp_int(game->leaks[i].route_id, 0, MAX_PATHS - 1);
        route_count_stats[r] += game->leaks[i].count;
        route_hp_stats[r] += game->leaks[i].hp_lost;
    }
    for (int r = 0; r < game->route_count; r++) {
        if (route_count_stats[r] > 0) {
            printf("  路线 %c: 漏 %d 只, 失血 %d\n",
                   'A' + r, route_count_stats[r], route_hp_stats[r]);
        }
    }

    printf("\n── 按敌人类型汇总 ──\n");
    int type_count_stats[MAX_ENEMY_TYPES] = {0};
    int type_hp_stats[MAX_ENEMY_TYPES] = {0};
    for (int i = 0; i < game->leak_count; i++) {
        int t = clamp_int(game->leaks[i].type, 0, MAX_ENEMY_TYPES - 1);
        type_count_stats[t] += game->leaks[i].count;
        type_hp_stats[t] += game->leaks[i].hp_lost;
    }
    for (int t = 0; t < MAX_ENEMY_TYPES; t++) {
        if (type_count_stats[t] > 0) {
            printf("  %-20s: 漏 %d 只, 失血 %d (%s)\n",
                   ENEMY_TYPE_NAMES[t], type_count_stats[t],
                   type_hp_stats[t], ENEMY_HP_COST_DESC[t]);
        }
    }

    printf("\n── 详细清单 ──\n");
    printf("  %-6s  %-6s  %-22s  %-6s  %-8s\n",
           "波次", "路线", "敌人类型", "漏怪数", "失血量");
    printf("  -----------------------------------------------------------\n");
    for (int i = 0; i < game->leak_count; i++) {
        printf("  %-8d  路线%-4c  %-22s  %-8d  %-8d\n",
               game->leaks[i].wave,
               'A' + game->leaks[i].route_id,
               ENEMY_TYPE_NAMES[game->leaks[i].type],
               game->leaks[i].count,
               game->leaks[i].hp_lost);
    }

    printf("\n── 平衡建议 ──\n");
    int worst_wave = 0, worst_wave_hp = 0;
    for (int w = 1; w <= game->max_waves; w++) {
        if (wave_hp_stats[w] > worst_wave_hp) {
            worst_wave_hp = wave_hp_stats[w];
            worst_wave = w;
        }
    }
    int worst_route = 0, worst_route_hp = 0;
    for (int r = 0; r < game->route_count; r++) {
        if (route_hp_stats[r] > worst_route_hp) {
            worst_route_hp = route_hp_stats[r];
            worst_route = r;
        }
    }
    int worst_type = 0, worst_type_hp = 0;
    for (int t = 0; t < MAX_ENEMY_TYPES; t++) {
        if (type_hp_stats[t] > worst_type_hp) {
            worst_type_hp = type_hp_stats[t];
            worst_type = t;
        }
    }
    printf("  ・压力最大波次: 第 %d 波 (失血 %d)，建议加强该波的火力覆盖\n",
           worst_wave, worst_wave_hp);
    printf("  ・压力最大路线: 路线 %c (失血 %d)，建议在此路径关键节点部署更多干员\n",
           'A' + worst_route, worst_route_hp);
    printf("  ・威胁最大敌人: %s (失血 %d)，%s\n",
           ENEMY_TYPE_NAMES[worst_type], worst_type_hp,
           worst_type == ENEMY_BOSS ? "建议用高等级干员集火首领" :
           worst_type == ENEMY_BULK ? "建议提升干员伤害应对高血量重装" :
           "建议在路径前中段部署干员快速清理");
    printf("  ・可输入 配置 进入关卡配置调整模式微调数值\n");

    printf("═══════════════════════════════════════════════════════════\n\n");
}

static void print_wave_preview(int wave) {
    WaveConfig cfg = get_wave_config(wave);
    printf("第 %d 波敌人预览：\n", wave);
    int total = 0;
    for (int g = 0; g < cfg.group_count; g++) {
        WaveGroup *grp = &cfg.groups[g];
        total += grp->count;
        printf("  路线%c: %-20s x %d (生命:%d, 速度每%d tick移1次, %s)\n",
               'A' + grp->route_id,
               ENEMY_TYPE_NAMES[grp->type],
               grp->count, grp->hp, grp->move_interval,
               ENEMY_HP_COST_DESC[grp->type]);
    }
    printf("  总计 %d 只敌人\n\n", total);
}

static void print_current_config(const Game *game) {
    printf("当前关卡配置：\n");
    printf("  基地初始生命: %d%s\n", game->initial_hp, game->hard_mode ? " (困难模式)" : "");
    printf("  初始费用: 20\n");
    printf("  总波次: %d\n", game->max_waves);
    printf("  路线数: %d\n", game->route_count);
    printf("  部署费用: 8, 升级费用: 10/16/22\n");
    printf("\n");
    for (int w = 1; w <= game->max_waves; w++) {
        print_wave_preview(w);
    }
}

static bool config_phase(Game *game) {
    char line[128];
    char prompt[32];
    clear_screen();
    printf("═══════════════════ 关卡配置调整模式 ═══════════════════\n");
    printf("当前基地生命: %d/%d, 费用: %d, 当前波次: %d/%d\n",
           game->hp, game->initial_hp, game->gold, game->wave, game->max_waves);
    printf("可用指令：\n");
    printf("  reset        重置游戏状态（费用=20, 波次=1, 基地满血）\n");
    printf("  hp N         设置基地初始/当前生命为 N\n");
    printf("  gold N       设置当前费用为 N\n");
    printf("  wave N       跳转到第 N 波（准备阶段）\n");
    printf("  max_waves N  设置总波次\n");
    printf("  preview [N]  预览第 N 波（或当前波）配置\n");
    printf("  show         显示完整关卡配置\n");
    printf("  continue     返回准备阶段继续游戏\n");
    printf("  quit         退出游戏\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    int empty_count = 0;
    while (true) {
        snprintf(prompt, sizeof(prompt), "配置> ");
        if (!read_command_line(prompt, line, sizeof(line))) {
            return false;
        }

        int n = 0;
        if (line[0] == '\n' || line[0] == '\0') {
            empty_count++;
            if (empty_count >= 3) {
                printf("连续空输入，自动返回准备阶段。\n");
                return true;
            }
            continue;
        }
        empty_count = 0;
        if (starts_with_icase(line, "reset")) {
            game->hp = game->initial_hp;
            game->gold = 20;
            game->wave = 1;
            game->leak_count = 0;
            game->total_hp_lost = 0;
            game->total_leaked_enemies = 0;
            for (int i = 0; i < MAX_ENEMIES; i++) game->enemies[i].alive = false;
            printf("已重置：基地满血, 费用=20, 波次=1, 清空漏怪记录\n");
        } else if (starts_with_icase(line, "hp ")) {
            if (sscanf(line, "%*s %d", &n) == 1 && n > 0) {
                game->initial_hp = n;
                game->hp = n;
                printf("基地初始/当前生命已设为 %d\n", n);
            } else {
                printf("用法: hp N (N>0)\n");
            }
        } else if (starts_with_icase(line, "gold ")) {
            if (sscanf(line, "%*s %d", &n) == 1 && n >= 0) {
                game->gold = n;
                printf("当前费用已设为 %d\n", n);
            } else {
                printf("用法: gold N (N>=0)\n");
            }
        } else if (starts_with_icase(line, "wave ")) {
            if (sscanf(line, "%*s %d", &n) == 1 && n >= 1 && n <= game->max_waves) {
                game->wave = n;
                for (int i = 0; i < MAX_ENEMIES; i++) game->enemies[i].alive = false;
                printf("已跳转到第 %d 波准备阶段\n", n);
            } else {
                printf("用法: wave N (1<=N<=%d)\n", game->max_waves);
            }
        } else if (starts_with_icase(line, "max_waves ")) {
            if (sscanf(line, "%*s %d", &n) == 1 && n >= 1 && n <= MAX_SUPPORTED_WAVES) {
                game->max_waves = n;
                if (game->wave > n) game->wave = n;
                printf("总波次已设为 %d\n", n);
            } else {
                printf("用法: max_waves N (1<=N<=%d)\n", MAX_SUPPORTED_WAVES);
            }
        } else if (starts_with_icase(line, "preview")) {
            int pw = game->wave;
            if (sscanf(line, "%*s %d", &n) == 1) pw = n;
            if (pw >= 1 && pw <= game->max_waves) {
                print_wave_preview(pw);
            } else {
                printf("波次范围: 1 - %d\n", game->max_waves);
            }
        } else if (starts_with_icase(line, "show")) {
            print_current_config(game);
        } else if (starts_with_icase(line, "continue")) {
            for (int i = 0; i < MAX_ENEMIES; i++) game->enemies[i].alive = false;
            return true;
        } else if (starts_with_icase(line, "quit")) {
            return false;
        } else if (line[0] == '\n' || line[0] == '\0') {
            continue;
        } else {
            printf("未知指令，输入 continue 返回准备阶段。\n");
        }
    }
}

static bool prep_phase(Game *game) {
    char line[128];
    char prompt[32];
    bool confirm_empty_start = false;
    print_help();
    print_wave_preview(game->wave);
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
        } else if (starts_with_icase(line, "types") || strncmp(line, "敌人", strlen("敌人")) == 0) {
            print_enemy_type_info();
            confirm_empty_start = false;
        } else if (starts_with_icase(line, "help") || strncmp(line, "帮助", strlen("帮助")) == 0) {
            print_help();
            confirm_empty_start = false;
        } else if (starts_with_icase(line, "tips") || strncmp(line, "提示", strlen("提示")) == 0) {
            print_tips();
            confirm_empty_start = false;
        } else if (starts_with_icase(line, "config") || strncmp(line, "配置", strlen("配置")) == 0) {
            if (!config_phase(game)) return false;
            clear_screen();
            render_board(game, false);
            printf("\n返回准备阶段：部署/升级后输入 start 或 开始 进入战斗。\n");
            print_wave_preview(game->wave);
            print_phase_hint(game);
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
    int group_spawned[MAX_ENEMY_TYPES * MAX_PATHS] = {0};
    int group_tick[MAX_ENEMY_TYPES * MAX_PATHS] = {0};
    int all_spawned = 0;
    int total_enemies = 0;
    for (int g = 0; g < cfg->group_count; g++) {
        total_enemies += cfg->groups[g].count;
    }
    int tick = 0;

    while (true) {
        for (int g = 0; g < cfg->group_count; g++) {
            const WaveGroup *grp = &cfg->groups[g];
            if (group_spawned[g] < grp->count) {
                if (group_tick[g] >= grp->spawn_interval || tick == 0) {
                    if (spawn_enemy(game, grp, group_spawned[g])) {
                        group_spawned[g] += 1;
                        all_spawned += 1;
                        group_tick[g] = 0;
                    }
                }
                group_tick[g] += 1;
            }
        }

        towers_attack(game);
        enemies_move(game, game->wave);

        clear_screen();
        render_board(game, true);
        printf("第 %d 波 | 战斗轮次: %d | 已出现: %d/%d\n", game->wave, tick, all_spawned, total_enemies);
        if (game->total_leaked_enemies > 0) {
            printf("本场漏怪: %d 只 (失血 %d) | 累计失血: %d\n",
                   game->total_leaked_enemies, game->total_hp_lost, game->total_hp_lost);
        }
        printf("战斗提示：自动战斗中，不同敌人到达终点扣不同血量，击杀敌人可获得费用。\n");

        if (game->hp <= 0) {
            return false;
        }

        if (all_spawned >= total_enemies && !has_live_enemies(game)) {
            game->gold += 5;
            printf("波次 %d 完成！额外获得 5 费用。\n", game->wave);
            sleep_ms(g_wave_clear_sleep_ms);
            return true;
        }

        tick += 1;
        sleep_ms(g_tick_sleep_ms);
    }
}

static bool post_game_menu(Game *game, bool victory) {
    char line[128];
    clear_screen();
    render_board(game, false);
    if (victory) {
        printf("\n恭喜通关！你成功守住了所有波次。\n");
    } else {
        printf("\n基地失守，游戏失败。\n");
    }
    print_leak_report(game);

    printf("战后选项：\n");
    printf("  1) config / 配置   - 进入关卡配置调整模式\n");
    printf("  2) restart / 重来  - 重新开始游戏（保留干员部署）\n");
    printf("  3) fullreset / 全重置 - 从头开始（清除所有干员）\n");
    printf("  4) quit / 退出     - 退出游戏\n");
    printf("\n请输入指令：\n");

    int empty_count = 0;
    while (true) {
        if (!read_command_line("战后> ", line, sizeof(line))) {
            return false;
        }
        if (starts_with_icase(line, "config") || strncmp(line, "配置", strlen("配置")) == 0) {
            if (!config_phase(game)) return false;
            return true;
        } else if (starts_with_icase(line, "restart") || strncmp(line, "重来", strlen("重来")) == 0) {
            game->hp = game->initial_hp;
            game->gold = 20;
            game->wave = 1;
            game->leak_count = 0;
            game->total_hp_lost = 0;
            game->total_leaked_enemies = 0;
            for (int i = 0; i < MAX_ENEMIES; i++) game->enemies[i].alive = false;
            return true;
        } else if (starts_with_icase(line, "fullreset") || strncmp(line, "全重置", strlen("全重置")) == 0) {
            Game saved;
            memcpy(&saved, game, sizeof(Game));
            init_game(game);
            memcpy(game->towers, saved.towers, sizeof(saved.towers));
            return true;
        } else if (starts_with_icase(line, "quit") || strncmp(line, "退出", strlen("退出")) == 0) {
            return false;
        } else if (line[0] == '\n' || line[0] == '\0') {
            empty_count++;
            if (empty_count >= 3) {
                printf("连续空输入，自动退出游戏。\n");
                return false;
            }
            continue;
        } else {
            empty_count = 0;
            printf("请选择: config(配置), restart(重来), fullreset(全重置), quit(退出)\n");
        }
    }
}

int main(void) {
    setlocale(LC_ALL, "");

    Game game;
    init_game(&game);

    clear_screen();
    printf("欢迎来到简化版明日方舟塔防（C 终端版）\n");
    printf("目标：守住 %d 波敌人，基地血量归零则失败。\n", game.max_waves);
    printf("初始基地生命: %d%s\n", game.initial_hp, game.hard_mode ? " (困难模式 TD_HARD=1)" : "");
    print_enemy_type_info();
    print_tips();

    while (true) {
        while (game.wave <= game.max_waves) {
            clear_screen();
            render_board(&game, false);
            printf("\n准备阶段：部署/升级后输入 start 或 开始 进入战斗。\n");
            if (!prep_phase(&game)) {
                printf("\n游戏结束（主动退出）。\n");
                return 0;
            }

            WaveConfig cfg = get_wave_config(game.wave);
            if (!run_wave(&game, &cfg)) {
                if (!post_game_menu(&game, false)) {
                    printf("\n游戏结束。\n");
                    return 0;
                }
                continue;
            }

            if (game.wave == game.max_waves) {
                break;
            }
            game.wave += 1;
        }

        if (!post_game_menu(&game, true)) {
            printf("\n游戏结束。\n");
            return 0;
        }
    }
}
