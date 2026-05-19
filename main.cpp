// =============================================================
//  SDL2 Task Manager — reads /proc for live process data
//  Build:  make
//  Run:    ./taskman
//
//  Controls:
//    Mouse wheel     — scroll process list
//    Click header    — sort by that column (click again to toggle ↑↓)
//    ESC / close     — quit
// =============================================================

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

//constants
static constexpr int   WIN_W        = 900;
static constexpr int   WIN_H        = 620;
static constexpr int   HEADER_H     = 56;   // title bar
static constexpr int   COL_HDR_H    = 30;   // column label row
static constexpr int   ROW_H        = 24;
static constexpr int   FOOTER_H     = 28;
static constexpr int   REFRESH_MS   = 500;
static constexpr float CPU_WARN     = 40.f;
static constexpr float CPU_CRIT     = 80.f;
static constexpr float MEM_WARN     = 40.f;
static constexpr float MEM_CRIT     = 80.f;

//colors
static const SDL_Color C_BG        = {10,  12,  10,  255};
static const SDL_Color C_PANEL     = {18,  24,  18,  255};
static const SDL_Color C_HDR_BG    = {14,  20,  14,  255};
static const SDL_Color C_ACCENT    = {57,  255, 20,  255};  // phosphor green
static const SDL_Color C_TEXT      = {200, 220, 200, 255};
static const SDL_Color C_DIM       = {90,  110, 90,  255};
static const SDL_Color C_GRID      = {25,  35,  25,  255};
static const SDL_Color C_OK        = {30,  160, 60,  255};
static const SDL_Color C_WARN      = {220, 180, 20,  255};
static const SDL_Color C_CRIT      = {220, 50,  30,  255};
static const SDL_Color C_SEL_ROW   = {22,  40,  22,  255};

//data types
struct ProcInfo {
    int         pid      = 0;
    std::string name;
    std::string user;
    float       cpu_pct  = 0.f;
    float       mem_pct  = 0.f;
    long        mem_kb   = 0;
    long        read_kb  = 0;
    long        write_kb = 0;
};

enum class SortCol { PID, NAME, CPU, MEM, READ, WRITE };

// /proc helpers

static long g_total_mem_kb = 1;

// Returns total CPU jiffies (user+nice+sys+idle+…)
static unsigned long long totalCpuJiffies() {
    std::ifstream f("/proc/stat");
    std::string token;
    f >> token;                     // "cpu"
    unsigned long long sum = 0, v;
    for (int i = 0; i < 10 && f >> v; ++i) sum += v;
    return sum;
}

// Returns per-process utime+stime jiffies
static unsigned long long procJiffies(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    if (!f) return 0;
    std::string tok;
    for (int i = 1; i <= 13; ++i) f >> tok;   // skip fields 1-13
    unsigned long long utime, stime;
    f >> utime >> stime;
    return utime + stime;
}

static std::string procName(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::string n;
    std::getline(f, n);
    return n.empty() ? "?" : n;
}

static std::string procUser(int pid) {
    // Read uid from /proc/<pid>/status
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Uid:", 0) == 0) {
            int uid = 0;
            sscanf(line.c_str(), "Uid:\t%d", &uid);
            // Simple uid -> name via /etc/passwd
            std::ifstream pw("/etc/passwd");
            std::string pl;
            while (std::getline(pw, pl)) {
                // format: name:x:uid:...
                int u = -1;
                char nm[64] = {};
                if (sscanf(pl.c_str(), "%63[^:]:%*[^:]:%d", nm, &u) == 2 && u == uid)
                    return std::string(nm);
            }
            return std::to_string(uid);
        }
    }
    return "?";
}

static long procMemKb(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            sscanf(line.c_str(), "VmRSS: %ld", &kb);
            return kb;
        }
    }
    return 0;
}

static void procIo(int pid, long &read_kb, long &write_kb) {
    read_kb = write_kb = 0;
    std::ifstream f("/proc/" + std::to_string(pid) + "/io");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        long v = 0;
        if (sscanf(line.c_str(), "read_bytes: %ld", &v) == 1)  read_kb  = v / 1024;
        if (sscanf(line.c_str(), "write_bytes: %ld", &v) == 1) write_kb = v / 1024;
    }
}

static long totalMemKb() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        long v = 0;
        if (sscanf(line.c_str(), "MemTotal: %ld", &v) == 1) return v;
    }
    return 1;
}

// integer-named entries in /proc -> PIDs
static std::vector<int> listPids() {
    std::vector<int> pids;
    DIR *d = opendir("/proc");
    if (!d) return pids;
    dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR || e->d_type == DT_UNKNOWN) {
            bool ok = true;
            for (char *p = e->d_name; *p; ++p)
                if (*p < '0' || *p > '9') { ok = false; break; }
            if (ok && e->d_name[0]) pids.push_back(atoi(e->d_name));
        }
    }
    closedir(d);
    return pids;
}

// renderer helpers
static void setColor(SDL_Renderer *r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static void fillRect(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c) {
    setColor(r, c);
    SDL_Rect rc{x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}

static void drawRect(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c) {
    setColor(r, c);
    SDL_Rect rc{x, y, w, h};
    SDL_RenderDrawRect(r, &rc);
}

static void drawHLine(SDL_Renderer *r, int x1, int x2, int y, SDL_Color c) {
    setColor(r, c);
    SDL_RenderDrawLine(r, x1, y, x2, y);
}

// Render text -> texture (caller frees)
static SDL_Texture *makeText(SDL_Renderer *r, TTF_Font *f,
                             const std::string &s, SDL_Color c) {
    if (s.empty()) return nullptr;
    SDL_Surface *sur = TTF_RenderUTF8_Blended(f, s.c_str(), c);
    if (!sur) return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, sur);
    SDL_FreeSurface(sur);
    return tex;
}

// Render text clipped to (x,y,maxW,h)
static void drawText(SDL_Renderer *r, TTF_Font *f,
                     const std::string &s, SDL_Color c,
                     int x, int y, int maxW = 9999) {
    SDL_Texture *tex = makeText(r, f, s, c);
    if (!tex) return;
    int tw, th;
    SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
    if (tw > maxW) tw = maxW;
    SDL_Rect src{0, 0, tw, th};
    SDL_Rect dst{x, y, tw, th};
    SDL_RenderCopy(r, tex, &src, &dst);
    SDL_DestroyTexture(tex);
}

// Bar: background track + filled portion coloured by value
static void drawBar(SDL_Renderer *r, int x, int y, int w, int h,
                    float pct, float warn, float crit) {
    SDL_Color track = {20, 30, 20, 255};
    fillRect(r, x, y, w, h, track);
    drawRect(r, x, y, w, h, {35, 50, 35, 255});
    int fill = static_cast<int>(pct / 100.f * w);
    if (fill < 1 && pct > 0) fill = 1;
    SDL_Color bar = (pct >= crit) ? C_CRIT : (pct >= warn) ? C_WARN : C_OK;
    if (fill > 0) fillRect(r, x, y, fill, h, bar);
}

// column layout
struct ColDef { const char *label; int x; int w; SortCol col; };

static const ColDef COLS[] = {
    {"PID",    4,   60,  SortCol::PID   },
    {"NAME",   68,  190, SortCol::NAME  },
    {"USER",   262, 90,  SortCol::NAME  },  // reuse NAME sort bucket
    {"CPU %",  356, 130, SortCol::CPU   },
    {"MEM %",  490, 130, SortCol::MEM   },
    {"READ KB",  624, 120, SortCol::READ  },
    {"WRT KB",   748, 120, SortCol::WRITE },
};
static constexpr int N_COLS = sizeof(COLS) / sizeof(COLS[0]);

int main() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 1;
    if (TTF_Init() != 0) { SDL_Quit(); return 1; }

    SDL_Window *win = SDL_CreateWindow(
        "Task Manager",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { TTF_Quit(); SDL_Quit(); return 1; }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit(); return 1; }

    // Load fonts — fall back gracefully
    auto tryFont = [](const char *path, int sz) -> TTF_Font* {
        TTF_Font *f = TTF_OpenFont(path, sz);
        return f;
    };
    const char *fontPaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        nullptr
    };
    TTF_Font *fontSm = nullptr, *fontMd = nullptr, *fontLg = nullptr;
    for (int i = 0; fontPaths[i] && !fontSm; ++i) {
        fontSm = tryFont(fontPaths[i], 13);
        fontMd = tryFont(fontPaths[i], 15);
        fontLg = tryFont(fontPaths[i], 22);
    }
    if (!fontSm) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Font missing",
            "Could not find a monospace font. Install fonts-dejavu-core.",
            win);
        return 1;
    }

    //state
    g_total_mem_kb = totalMemKb();
    std::vector<ProcInfo> procs;
    SortCol sortCol  = SortCol::CPU;
    bool    sortAsc  = false;
    int     scrollY  = 0;   // pixel offset into list
    int     hovRow   = -1;

    // CPU delta tracking: pid -> (last_proc_jiffies, last_total_jiffies)
    std::map<int, std::pair<unsigned long long, unsigned long long>> cpuSnap;

    auto now_ms = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    long long lastRefresh = 0;

    auto refresh = [&]() {
        unsigned long long totalNow = totalCpuJiffies();
        std::vector<int> pids = listPids();
        std::vector<ProcInfo> next;
        next.reserve(pids.size());

        for (int pid : pids) {
            ProcInfo p;
            p.pid  = pid;
            p.name = procName(pid);
            p.user = procUser(pid);

            unsigned long long pjNow = procJiffies(pid);
            auto it = cpuSnap.find(pid);
            if (it != cpuSnap.end()) {
                unsigned long long dpj = pjNow - it->second.first;
                unsigned long long dtj = totalNow - it->second.second;
                p.cpu_pct = (dtj > 0)
                    ? static_cast<float>(dpj * 100.0 / dtj)
                    : 0.f;
            }
            cpuSnap[pid] = {pjNow, totalNow};

            p.mem_kb  = procMemKb(pid);
            p.mem_pct = (g_total_mem_kb > 0)
                ? static_cast<float>(p.mem_kb * 100.0 / g_total_mem_kb)
                : 0.f;
            procIo(pid, p.read_kb, p.write_kb);

            next.push_back(std::move(p));
        }
        procs = std::move(next);
    };

    auto sortProcs = [&]() {
        std::sort(procs.begin(), procs.end(),
            [&](const ProcInfo &a, const ProcInfo &b) {
                bool less = false;
                switch (sortCol) {
                    case SortCol::PID:   less = a.pid     < b.pid;     break;
                    case SortCol::NAME:  less = a.name    < b.name;    break;
                    case SortCol::CPU:   less = a.cpu_pct < b.cpu_pct; break;
                    case SortCol::MEM:   less = a.mem_pct < b.mem_pct; break;
                    case SortCol::READ:  less = a.read_kb < b.read_kb; break;
                    case SortCol::WRITE: less = a.write_kb< b.write_kb;break;
                }
                return sortAsc ? less : !less;
            });
    };

    // event loop
    bool running = true;
    SDL_Event ev;

    while (running) {
        // handle events
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = false; break; }

            if (ev.type == SDL_KEYDOWN &&
                ev.key.keysym.sym == SDLK_ESCAPE) { running = false; break; }

            if (ev.type == SDL_MOUSEWHEEL) {
                scrollY -= ev.wheel.y * ROW_H * 3;
                if (scrollY < 0) scrollY = 0;
            }

            if (ev.type == SDL_MOUSEMOTION) {
                int mx = ev.motion.x, my = ev.motion.y;
                int listTop = HEADER_H + COL_HDR_H;
                int listBot = WIN_H - FOOTER_H;
                if (my >= listTop && my < listBot) {
                    hovRow = (my - listTop + scrollY) / ROW_H;
                } else {
                    hovRow = -1;
                }
            }

            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                int mx = ev.button.x, my = ev.button.y;
                // Click column header?
                if (my >= HEADER_H && my < HEADER_H + COL_HDR_H) {
                    for (int c = 0; c < N_COLS; ++c) {
                        if (mx >= COLS[c].x && mx < COLS[c].x + COLS[c].w) {
                            if (sortCol == COLS[c].col) sortAsc = !sortAsc;
                            else { sortCol = COLS[c].col; sortAsc = false; }
                            sortProcs();
                            break;
                        }
                    }
                }
            }
        }

        // refresh data at interval
        long long t = now_ms();
        if (t - lastRefresh >= REFRESH_MS) {
            lastRefresh = t;
            refresh();
            sortProcs();
            // Clamp scroll
            int maxScroll = std::max(0, (int)procs.size() * ROW_H - (WIN_H - HEADER_H - COL_HDR_H - FOOTER_H));
            if (scrollY > maxScroll) scrollY = maxScroll;
        }

        //render
        int winW, winH;
        SDL_GetWindowSize(win, &winW, &winH);
        int listH = winH - HEADER_H - COL_HDR_H - FOOTER_H;

        setColor(ren, C_BG);
        SDL_RenderClear(ren);

        // Title bar
        fillRect(ren, 0, 0, winW, HEADER_H, C_HDR_BG);
        drawHLine(ren, 0, winW, HEADER_H - 1, C_ACCENT);

        // Accent left stripe
        fillRect(ren, 0, 0, 4, HEADER_H, C_ACCENT);

        drawText(ren, fontLg, "TASK MANAGER", C_ACCENT, 16, 10);
        drawText(ren, fontSm, "/proc live view  •  " + std::to_string(procs.size()) + " processes",
                 C_DIM, 16, 36);

        // Column header row
        fillRect(ren, 0, HEADER_H, winW, COL_HDR_H, C_PANEL);
        drawHLine(ren, 0, winW, HEADER_H + COL_HDR_H - 1, C_GRID);

        for (int c = 0; c < N_COLS; ++c) {
            bool active = (COLS[c].col == sortCol);
            SDL_Color lc = active ? C_ACCENT : C_DIM;
            std::string label = COLS[c].label;
            if (active) label += (sortAsc ? " ▲" : " ▼");
            drawText(ren, fontSm, label, lc, COLS[c].x + 2, HEADER_H + 8, COLS[c].w - 4);
            // column separator
            if (c > 0) drawHLine(ren, COLS[c].x, COLS[c].x, HEADER_H, HEADER_H + COL_HDR_H, C_GRID);
        }

        // Clip list area (software clip via scissor)
        SDL_Rect clip{0, HEADER_H + COL_HDR_H, winW, listH};
        SDL_RenderSetClipRect(ren, &clip);

        int y0 = HEADER_H + COL_HDR_H - scrollY;
        for (int i = 0; i < (int)procs.size(); ++i) {
            int ry = y0 + i * ROW_H;
            if (ry + ROW_H < HEADER_H + COL_HDR_H) continue;
            if (ry > winH - FOOTER_H) break;

            const ProcInfo &p = procs[i];
            bool hov = (i == hovRow);

            // Row background
            SDL_Color rowBg = (i % 2 == 0) ? C_BG : C_PANEL;
            if (hov) rowBg = C_SEL_ROW;
            fillRect(ren, 0, ry, winW, ROW_H, rowBg);
            drawHLine(ren, 0, winW, ry + ROW_H - 1, C_GRID);

            int ty = ry + (ROW_H - 13) / 2;   // vertically centred text

            // PID
            drawText(ren, fontSm, std::to_string(p.pid), C_DIM, COLS[0].x + 2, ty, COLS[0].w - 4);

            // NAME
            drawText(ren, fontSm, p.name, C_TEXT, COLS[1].x + 2, ty, COLS[1].w - 4);

            // USER
            drawText(ren, fontSm, p.user, C_DIM, COLS[2].x + 2, ty, COLS[2].w - 4);

            // CPU bar + label
            {
                int bx = COLS[3].x + 2, bw = COLS[3].w - 40;
                drawBar(ren, bx, ry + 5, bw, ROW_H - 10,
                        p.cpu_pct, CPU_WARN, CPU_CRIT);
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << p.cpu_pct << "%";
                SDL_Color tc = (p.cpu_pct >= CPU_CRIT) ? C_CRIT
                             : (p.cpu_pct >= CPU_WARN) ? C_WARN
                             : C_TEXT;
                drawText(ren, fontSm, ss.str(), tc,
                         bx + bw + 3, ty, 36);
            }

            // MEM bar + label
            {
                int bx = COLS[4].x + 2, bw = COLS[4].w - 45;
                drawBar(ren, bx, ry + 5, bw, ROW_H - 10,
                        p.mem_pct, MEM_WARN, MEM_CRIT);
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << p.mem_pct << "%";
                SDL_Color tc = (p.mem_pct >= MEM_CRIT) ? C_CRIT
                             : (p.mem_pct >= MEM_WARN) ? C_WARN
                             : C_TEXT;
                drawText(ren, fontSm, ss.str(), tc,
                         bx + bw + 3, ty, 40);
            }

            // READ KB
            drawText(ren, fontSm,
                     p.read_kb > 0 ? std::to_string(p.read_kb) : "-",
                     C_DIM, COLS[5].x + 2, ty, COLS[5].w - 4);

            // WRITE KB
            drawText(ren, fontSm,
                     p.write_kb > 0 ? std::to_string(p.write_kb) : "-",
                     C_DIM, COLS[6].x + 2, ty, COLS[6].w - 4);
        }

        SDL_RenderSetClipRect(ren, nullptr);

        // Footer
        fillRect(ren, 0, winH - FOOTER_H, winW, FOOTER_H, C_HDR_BG);
        drawHLine(ren, 0, winW, winH - FOOTER_H, C_ACCENT);

        // Global memory usage in footer
        {
            long usedKb  = 0;
            for (auto &p : procs) usedKb += p.mem_kb;
            float usedPct = (g_total_mem_kb > 0)
                ? static_cast<float>(usedKb * 100.0 / g_total_mem_kb)
                : 0.f;
            std::ostringstream ss;
            ss << "Total RAM: " << g_total_mem_kb / 1024 << " MB"
               << "   Used: " << usedKb / 1024 << " MB"
               << "  (" << std::fixed << std::setprecision(1) << usedPct << "%)"
               << "   Refresh: " << REFRESH_MS << " ms"
               << "   Scroll: mouse wheel   Sort: click header";
            drawText(ren, fontSm, ss.str(), C_DIM, 8, winH - FOOTER_H + 8);
        }

        SDL_RenderPresent(ren);
        SDL_Delay(16);  // ~60 fps render loop; data refreshed every REFRESH_MS
    }

    TTF_CloseFont(fontSm);
    TTF_CloseFont(fontMd);
    TTF_CloseFont(fontLg);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
