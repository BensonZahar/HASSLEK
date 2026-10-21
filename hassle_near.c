/*
 * hassle_near.c — мод «игроки рядом» для Hassle Online (IL2CPP, Unity).
 *
 * Как это работает
 *   • Два режима загрузки (patch_apk.py выбирает сам):
 *     1) Старый клиент: библиотека называется libHassleExtended.so — её уже грузит игра (System.loadLibrary("HassleExtended")).
 *        Оригинальный мод переименован в libHassleExtend2.so; из нашего JNI_OnLoad он загружается и получает тот же вызов.
 *     2) Чистый APK (Unity 2021.3, libHassleExtended.so в нём нет): мы подменяем libmain.so — её игра грузит первой
 *        (System.loadLibrary("main")). Оригинальный libmain.so переименован в libHassleMain2.so; наш JNI_OnLoad грузит его
 *        и передаёт ему тот же вызов — он, как и раньше, регистрирует NativeLoader и сам поднимает libunity/libil2cpp.
 *   • Фоновый поток присоединяется к IL2CPP (il2cpp_thread_attach), находит Hassle.Global.Pooling.PlayerPool и раз в 300 мс
 *     читает PlayersInStream (игроки, которых клиент «видит» рядом): ID, ник из подписи над головой, позицию из
 *     последнего пакета синхронизации, HP/броню, пинг и счёт.
 *   • Второй поток — крошечный HTTP-сервер на 127.0.0.1:17771 (порты 17771..17780). HUD (Code3.js) опрашивает
 *     http://127.0.0.1:17771/near  и получает JSON. Ничего из IL2CPP на главный поток не навешивается — игру это не трогает.
 *   • Реконнект: GET /rec?t=N (N = секунд до подключения, 0..600, по умолчанию 0 = сразу) делает то же, что команда /rec оригинального
 *     мода: Game.Network.OnApplicationQuit() (шлёт серверу ID_DISCONNECTION_NOTIFICATION, если State == Connected), затем
 *     Game.Network.DelayedConnect(m_Host, m_Port, m_Password, m_NickName, N). В Code3.js — команда /rec [секунд] (/rec2 — синоним).
 *
 * Версия без паузы и без JS для /rec:
 *   • Вместо слепых 60 секунд сборщик ждёт РЕАЛЬНУЮ готовность игры: libil2cpp загружена → виден Assembly-CSharp →
 *     (короткая стабилизация SETTLE_MS) → создан Hassle.Game.Instance. Пока ждёт, IL2CPP не трогает дальше лёгкого опроса.
 *   • /rec работает и без сборщика: GET /rec сам лениво инициализирует IL2CPP (набрать команду можно только когда игра уже идёт).
 *   • Нативная чат-команда: мод регистрирует «/rec» прямо в реестре команд игры (WebViewSystem.m_ChatCommandEvents), так что
 *     ни rec.js, ни HTTP-порты для /rec не нужны. Делегат клонируется с уже зарегистрированной игрой команды, подменяется method_ptr.
 *
 * Только IL2CPP C API (экспортируется libil2cpp.so) — исходники игры и BNM не нужны, классы ищутся по имени.
 * Код не использует libc-заголовки: прототипы объявлены вручную, поэтому собирается обычным clang без NDK.
 */

typedef unsigned long uptr;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define EXPORT __attribute__((visibility("default")))

/* ───────────── libc / libdl (резолвятся загрузчиком; DT_NEEDED даёт заглушка при линковке) ───────────── */
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int socket(int, int, int);
extern int setsockopt(int, int, int, const void*, unsigned);
extern int bind(int, const void*, unsigned);
extern int listen(int, int);
extern int accept(int, void*, void*);
extern long recv(int, void*, unsigned long, int);
extern long send(int, const void*, unsigned long, int);
extern int close(int);
extern int pthread_create(unsigned long*, const void*, void* (*)(void*), void*);
extern int usleep(unsigned);
extern int open(const char*, int, ...);
extern long read(int, void*, unsigned long);
extern long time(long*);
extern void* memcpy(void*, const void*, unsigned long);
extern int mkdir(const char*, unsigned);
extern long write(int, const void*, unsigned long);
extern int rename(const char*, const char*);
extern char* dlerror(void);

#define RTLD_NOW_ 2
#define NEAR_PORT_FIRST 17771
#define NEAR_PORT_COUNT 10
#define TICK_US 300000
#ifndef NEAR_EXTRA_DELAY_S
#define NEAR_EXTRA_DELAY_S 0  /* запасной рычаг: доп. пауза после готовности игры. Если вдруг вернутся вылеты на старте — собрать с -DNEAR_EXTRA_DELAY_S=10 */
#endif
#define SETTLE_MS 1500        /* пауза после того, как стал виден Assembly-CSharp (доинициализация IL2CPP), до первого присоединения потока */
#define POLL_US 250000        /* период лёгкого опроса готовности */

/* ───────────── мелкие строковые помощники (без libc) ───────────── */
static unsigned slen(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int sprefix(const char* s, const char* p) { while (*p) { if (*s != *p) return 0; s++; p++; } return 1; }
static int ends_with(const char* s, const char* suf) {
    unsigned a = slen(s), b = slen(suf);
    return a >= b && seq(s + a - b, suf);
}

typedef struct { char* p; unsigned n, cap; } Buf;
static void b_init(Buf* b, char* mem, unsigned cap) { b->p = mem; b->n = 0; b->cap = cap; if (cap) mem[0] = 0; }
static void b_ch(Buf* b, char c) { if (b->n + 1 < b->cap) { b->p[b->n++] = c; b->p[b->n] = 0; } }
static void b_s(Buf* b, const char* s) { while (s && *s) b_ch(b, *s++); }
static void b_int(Buf* b, long v) {
    char t[24]; int i = 0; unsigned long u;
    if (v < 0) { b_ch(b, '-'); u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + (u % 10)); u /= 10; }
    while (i) b_ch(b, t[--i]);
}
static void b_fix(Buf* b, double v, int dec) {          /* число с dec знаками после запятой */
    int i; double m = 1;
    if (!(v == v) || v > 1e12 || v < -1e12) { b_s(b, "0"); return; }
    if (v < 0) { b_ch(b, '-'); v = -v; }
    for (i = 0; i < dec; i++) m *= 10;
    {
        unsigned long long all = (unsigned long long)(v * m + 0.5);
        unsigned long long ip = all / (unsigned long long)m, fp = all % (unsigned long long)m;
        char t[24]; int k = 0;
        if (!ip) t[k++] = '0';
        while (ip) { t[k++] = (char)('0' + (ip % 10)); ip /= 10; }
        while (k) b_ch(b, t[--k]);
        if (dec) {
            b_ch(b, '.');
            for (i = dec - 1; i >= 0; i--) {
                unsigned long long d = fp, q = 1; int j;
                for (j = 0; j < i; j++) q *= 10;
                d = (d / q) % 10;
                b_ch(b, (char)('0' + d));
            }
        }
    }
}
static void b_jstr(Buf* b, const char* s) {            /* JSON-строка (s — UTF-8) */
    b_ch(b, '"');
    for (; s && *s; s++) {
        u8 c = (u8)*s;
        if (c == '"' || c == '\\') { b_ch(b, '\\'); b_ch(b, (char)c); }
        else if (c < 0x20) { b_s(b, "\\u00"); b_ch(b, "0123456789abcdef"[c >> 4]); b_ch(b, "0123456789abcdef"[c & 15]); }
        else b_ch(b, (char)c);
    }
    b_ch(b, '"');
}

/* ───────────── журнал диагностики (виден по /diag) ───────────── */
static char g_diag_mem[24576];
static Buf g_diag;
static int g_diag_ready;
typedef void (*logfn_t)(int, const char*, const char*);
static logfn_t g_alog;

/* ───────────── файловый журнал: <папка игры>/files/HLOG/near.log ─────────────
   Каждая строка дописывается в файл сразу же (open → write → close), поэтому при вылете последняя строка показывает, где всё оборвалось.
   Основной путь: /sdcard/Android/data/<пакет>/files/HLOG/near.log  (Android/data/<пакет>/files/HLOG в файловом менеджере/по USB).
   Если туда писать нельзя — запасной: /data/data/<пакет>/files/HLOG/near.log. Прошлый запуск сохраняется как near_prev.log. */
static char g_flog[300], g_flog_prev[300];
static int g_flogInit, g_flogLines, g_flogFallback;
static volatile int g_flogLock;
static long g_flogT0;

static int pkg_name(char* out, int cap) {
    char cmd[128]; int fd, i; long n;
    out[0] = 0;
    fd = open("/proc/self/cmdline", 0);
    if (fd < 0) return 0;
    n = read(fd, cmd, sizeof cmd - 1); close(fd);
    if (n <= 0) return 0;
    cmd[n] = 0;
    for (i = 0; cmd[i] && cmd[i] != ':' && i < cap - 1; i++) out[i] = cmd[i];
    out[i] = 0;
    return i > 0;
}
static void flog_paths(const char* root, const char* pkg) {      /* root: "/sdcard/Android/data/" или "/data/data/" */
    char d[300]; Buf b;
    b_init(&b, d, sizeof d); b_s(&b, root); b_s(&b, pkg); mkdir(d, 0777);
    b_s(&b, "/files"); mkdir(d, 0777);
    b_s(&b, "/HLOG"); mkdir(d, 0777);
    b_init(&b, g_flog, sizeof g_flog); b_s(&b, d); b_s(&b, "/near.log");
    b_init(&b, g_flog_prev, sizeof g_flog_prev); b_s(&b, d); b_s(&b, "/near_prev.log");
}
static int flog_write(const char* path, const char* data, unsigned n, int create_new) {
    int fd = open(path, create_new ? 0x241 : 0x441, 0666);        /* O_WRONLY|O_CREAT|(O_TRUNC | O_APPEND) */
    if (fd < 0) return 0;
    write(fd, data, n); close(fd);
    return 1;
}
static void file_log(const char* line) {
    char t[700]; Buf b; long now;
    while (__sync_lock_test_and_set(&g_flogLock, 1)) usleep(200);
    if (g_flogLines > 4000) { __sync_lock_release(&g_flogLock); return; }
    now = time(NULL);
    if (!g_flogInit) {
        char pkg[100];
        g_flogInit = 1; g_flogT0 = now;
        if (pkg_name(pkg, sizeof pkg)) {
            flog_paths("/sdcard/Android/data/", pkg);
            rename(g_flog, g_flog_prev);                          /* прошлый запуск (мог закончиться вылетом) не теряем */
            if (!flog_write(g_flog, "", 0, 1)) {                  /* внешняя папка недоступна — запасная внутренняя */
                flog_paths("/data/data/", pkg); g_flogFallback = 1;
                rename(g_flog, g_flog_prev);
                if (!flog_write(g_flog, "", 0, 1)) g_flog[0] = 0;
            }
        } else g_flog[0] = 0;
        if (g_flog[0]) {
            b_init(&b, t, sizeof t); b_s(&b, "== запуск мода Near; файл: "); b_s(&b, g_flog);
            if (g_flogFallback) b_s(&b, "  (ВНУТРЕННЯЯ папка: в /sdcard/Android/data писать нельзя)");
            b_s(&b, "\n"); flog_write(g_flog, t, b.n, 0);
        }
    }
    if (g_flog[0]) {
        b_init(&b, t, sizeof t);
        b_ch(&b, '+'); b_int(&b, now - g_flogT0); b_s(&b, "с  "); b_s(&b, line); b_ch(&b, '\n');
        flog_write(g_flog, t, b.n, 0);
        g_flogLines++;
    }
    __sync_lock_release(&g_flogLock);
}

static void dlog(const char* s) {
    if (!g_diag_ready) { b_init(&g_diag, g_diag_mem, sizeof g_diag_mem); g_diag_ready = 1; }
    if (g_diag.n + 200 > g_diag.cap) {                  /* кольцо: когда заполнилось — начинаем сначала */
        g_diag.n = 0; g_diag.p[0] = 0;
        b_s(&g_diag, "[журнал переполнен, начат заново]\n");
    }
    b_s(&g_diag, s); b_ch(&g_diag, '\n');
    if (g_alog) g_alog(4, "HassleNear", s);
    file_log(s);
}
static void dlog2(const char* a, const char* b) {
    char t[512]; Buf x; b_init(&x, t, sizeof t); b_s(&x, a); b_s(&x, b); dlog(t);
}
static void dlog_i(const char* a, long v) {
    char t[512]; Buf x; b_init(&x, t, sizeof t); b_s(&x, a); b_int(&x, v); dlog(t);
}

/* ───────────── IL2CPP API ───────────── */
static struct {
    void* (*domain_get)(void);
    void* (*thread_attach)(void*);
    void** (*domain_get_assemblies)(void*, uptr*);
    void* (*assembly_get_image)(void*);
    const char* (*image_get_name)(void*);
    uptr (*image_get_class_count)(void*);
    void* (*image_get_class)(void*, uptr);
    void* (*class_from_name)(void*, const char*, const char*);
    const char* (*class_get_name)(void*);
    const char* (*class_get_namespace)(void*);
    void* (*class_get_parent)(void*);
    void* (*class_get_fields)(void*, void**);
    void* (*class_get_field_from_name)(void*, const char*);
    const char* (*field_get_name)(void*);
    int (*field_get_flags)(void*);
    void* (*field_get_type)(void*);
    int (*field_get_offset)(void*);
    void (*field_static_get_value)(void*, void*);
    void* (*object_get_class)(void*);
    int (*type_get_type)(void*);
    void* (*class_from_type)(void*);
    int (*class_is_valuetype)(void*);
    void* (*class_get_method_from_name)(void*, const char*, int);
    void* (*runtime_invoke)(void*, void*, void**, void**);
    u16* (*string_chars)(void*);
    int (*string_length)(void*);
    int (*class_array_element_size)(void*);
    void* (*class_get_element_class)(void*);
    const char* (*type_get_name)(void*);
    /* необязательные (нужны только для нативной команды /rec) */
    void* (*string_new)(const char*);
    void* (*object_new)(void*);
    int (*class_instance_size)(void*);
} A;

static void* g_il2cpp;
static void* g_domain;

#define LOAD(n) do { *(void**)&A.n = dlsym(g_il2cpp, "il2cpp_" #n); if (!A.n) { missing = #n; } } while (0)

static int il2cpp_open(void) {
    const char* missing = NULL;
    if (g_il2cpp) return 1;
#ifdef NEAR_TEST
    g_il2cpp = dlopen(NULL, RTLD_NOW_);
#else
    g_il2cpp = dlopen("libil2cpp.so", RTLD_NOW_ | 4);   /* RTLD_NOLOAD: только если игра уже загрузила, сами не грузим */
#endif
    if (!g_il2cpp) return 0;
    LOAD(domain_get); LOAD(thread_attach); LOAD(domain_get_assemblies); LOAD(assembly_get_image);
    LOAD(image_get_name); LOAD(image_get_class_count); LOAD(image_get_class); LOAD(class_from_name);
    LOAD(class_get_name); LOAD(class_get_namespace); LOAD(class_get_parent); LOAD(class_get_fields);
    LOAD(class_get_field_from_name); LOAD(field_get_name); LOAD(field_get_flags); LOAD(field_get_type);
    LOAD(field_get_offset); LOAD(field_static_get_value); LOAD(object_get_class); LOAD(type_get_type);
    LOAD(class_from_type); LOAD(class_is_valuetype); LOAD(class_get_method_from_name); LOAD(runtime_invoke);
    LOAD(string_chars); LOAD(string_length); LOAD(class_array_element_size); LOAD(class_get_element_class);
    LOAD(type_get_name);
    *(void**)&A.string_new = dlsym(g_il2cpp, "il2cpp_string_new");
    *(void**)&A.object_new = dlsym(g_il2cpp, "il2cpp_object_new");
    *(void**)&A.class_instance_size = dlsym(g_il2cpp, "il2cpp_class_instance_size");
    if (missing) { dlog2("нет функции il2cpp_", missing); g_il2cpp = NULL; return 0; }
    return 1;
}

/* ───────────── доступ к полям ───────────── */
#define HDR (2 * sizeof(void*))                   /* Il2CppObject: klass + monitor */

static void* find_field(void* k, const char* name) {
    for (; k; k = A.class_get_parent(k)) {
        void* f = A.class_get_field_from_name(k, name);
        if (f) return f;
    }
    return NULL;
}
static void* find_method(void* k, const char* name, int argc) {
    for (; k; k = A.class_get_parent(k)) {
        void* m = A.class_get_method_from_name(k, name, argc);
        if (m) return m;
    }
    return NULL;
}
static void* ref_field(void* obj, const char* name) {   /* значение ссылочного поля объекта */
    void* f; void* v = NULL;
    if (!obj) return NULL;
    f = find_field(A.object_get_class(obj), name);
    if (!f) return NULL;
    v = *(void**)((char*)obj + A.field_get_offset(f));
    return v;
}
static int field_is_valuetype(void* f) {
    void* t = A.field_get_type(f);
    void* c = t ? A.class_from_type(t) : NULL;
    return c ? A.class_is_valuetype(c) : 0;
}
static double read_num(void* addr, int t) {
    switch (t) {
        case 4: return *(signed char*)addr;
        case 5: return *(u8*)addr;
        case 6: return *(short*)addr;
        case 7: return *(u16*)addr;
        case 8: return *(int*)addr;
        case 9: return *(u32*)addr;
        case 10: return (double)*(long long*)addr;
        case 11: return (double)*(unsigned long long*)addr;
        case 12: return *(float*)addr;
        case 13: return *(double*)addr;
        default: return 0;
    }
}
/* число из поля объекта; ok=0 если поля нет */
static double num_field(void* obj, const char* name, int* ok) {
    void* f;
    if (ok) *ok = 0;
    if (!obj) return 0;
    f = find_field(A.object_get_class(obj), name);
    if (!f) return 0;
    if (ok) *ok = 1;
    return read_num((char*)obj + A.field_get_offset(f), A.type_get_type(A.field_get_type(f)));
}
/* смещение первого поля структуры: у value-типов в метаданных оно бывает с учётом заголовка объекта */
static int struct_shift(void* sclass) {
    void* it = NULL; void* f; int minoff = 0x7fffffff;
    while ((f = A.class_get_fields(sclass, &it))) {
        if (A.field_get_flags(f) & 0x10) continue;
        if (A.field_get_offset(f) < minoff) minoff = A.field_get_offset(f);
    }
    return (minoff != 0x7fffffff && minoff >= (int)HDR) ? (int)HDR : 0;
}

/* ───────────── поиск классов ───────────── */
static void* g_img;
static void* g_cPools, *g_cPlayerPool;
static void* g_poolSrcField;       /* static-поле, в котором лежит PlayerPool (или Pools) */
static int g_poolSrcIsPools;
static int g_fieldsDumped;

static void* find_image(void) {
    uptr n = 0, i; void** as;
    if (!g_domain) return NULL;
    as = A.domain_get_assemblies(g_domain, &n);
    for (i = 0; as && i < n; i++) {
        void* img = A.assembly_get_image(as[i]);
        const char* nm = img ? A.image_get_name(img) : NULL;
        if (nm && sprefix(nm, "Assembly-CSharp") && !sprefix(nm, "Assembly-CSharp-")) return img;
    }
    return NULL;
}

static __thread int tl_attached;
static void ensure_attached(void) {
    if (!tl_attached && g_domain) { A.thread_attach(g_domain); tl_attached = 1; }
}
/* Лёгкая проверка: libil2cpp загружена, домен создан, Assembly-CSharp виден. Классов не трогает, поток не присоединяет. */
static int il2cpp_light(void) {
    if (!il2cpp_open()) return 0;
    if (!g_domain) g_domain = A.domain_get();
    if (!g_domain) return 0;
    if (!g_img) g_img = find_image();
    return g_img != NULL;
}
static void* g_cGame, *g_fGameInstance;
/* Игра запущена: Hassle.Game.Instance создан (Game.Awake отработал). Нужен присоединённый поток. */
static int game_ready(void) {
    void* inst = NULL;
    if (!il2cpp_light()) return 0;
    if (!g_cGame) g_cGame = A.class_from_name(g_img, "Hassle", "Game");
    if (!g_cGame) return 0;
    if (!g_fGameInstance) g_fGameInstance = A.class_get_field_from_name(g_cGame, "Instance");
    if (!g_fGameInstance) return 0;
    ensure_attached();
    A.field_static_get_value(g_fGameInstance, &inst);
    return inst != NULL;
}

static void dump_class(void* k, const char* title) {
    void* it = NULL; void* f; int guard = 0;
    char t[600]; Buf b;
    b_init(&b, t, sizeof t); b_s(&b, "== "); b_s(&b, title); b_s(&b, " ("); b_s(&b, A.class_get_name(k)); b_s(&b, ")");
    dlog(t);
    for (; k && guard < 6; k = A.class_get_parent(k), guard++) {
        it = NULL;
        while ((f = A.class_get_fields(k, &it))) {
            void* ty = A.field_get_type(f);
            b_init(&b, t, sizeof t);
            b_s(&b, "   "); b_s(&b, A.class_get_name(k)); b_s(&b, "."); b_s(&b, A.field_get_name(f));
            b_s(&b, " : "); b_s(&b, ty ? A.type_get_name(ty) : "?");
            b_s(&b, " @"); b_int(&b, A.field_get_offset(f));
            if (A.field_get_flags(f) & 0x10) b_s(&b, " static");
            dlog(t);
        }
    }
}

/* PlayerPool может лежать в статическом поле Pools.Players, либо в статическом поле любого класса;
   запоминаем найденное static-поле и каждый тик читаем из него свежее значение. */
static void* current_pool(void) {
    void* v = NULL;
    if (!g_poolSrcField) return NULL;
    A.field_static_get_value(g_poolSrcField, &v);
    if (!v) return NULL;
    if (g_poolSrcIsPools) return ref_field(v, "Players");
    return v;
}
static void scan_for_pool(void) {
    uptr n, i;
    void* it; void* f;
    /* 1) Pools.Players как статическое поле */
    if (g_cPools) {
        f = A.class_get_field_from_name(g_cPools, "Players");
        if (f && (A.field_get_flags(f) & 0x10)) { g_poolSrcField = f; g_poolSrcIsPools = 0; dlog("PlayerPool: Pools.Players (static)"); return; }
    }
    /* 2) любое static-поле типа PlayerPool или Pools */
    n = A.image_get_class_count(g_img);
    for (i = 0; i < n; i++) {
        void* k = A.image_get_class(g_img, i);
        if (!k) continue;
        it = NULL;
        while ((f = A.class_get_fields(k, &it))) {
            void* ty; void* c;
            if (!(A.field_get_flags(f) & 0x10) || (A.field_get_flags(f) & 0x40)) continue;
            ty = A.field_get_type(f);
            if (!ty || A.type_get_type(ty) != 0x12) continue;      /* CLASS */
            c = A.class_from_type(ty);
            if (c == g_cPlayerPool) {
                g_poolSrcField = f; g_poolSrcIsPools = 0;
                dlog2("PlayerPool: static-поле ", A.field_get_name(f)); dlog2("   в классе ", A.class_get_name(k));
                return;
            }
            if (c == g_cPools) {
                g_poolSrcField = f; g_poolSrcIsPools = 1;
                dlog2("Pools: static-поле ", A.field_get_name(f)); dlog2("   в классе ", A.class_get_name(k));
                return;
            }
        }
    }
}

/* ───────────── перебор коллекции ───────────── */
typedef void (*item_cb)(void* item, void* ctx);

static void walk_ref_array(void* arr, int count, int stride_is_ptr, item_cb cb, void* ctx) {
    int i; char* data;
    if (!arr) return;
    {
        long len = *(long*)((char*)arr + 3 * sizeof(void*));
        if (len < 0 || len > 100000) return;
        if (count < 0 || count > len) count = (int)len;
    }
    data = (char*)arr + 4 * sizeof(void*);
    (void)stride_is_ptr;
    for (i = 0; i < count; i++) {
        void* it = *(void**)(data + (uptr)i * sizeof(void*));
        if (it) cb(it, ctx);
    }
}
/* массив структур (записи Dictionary/HashSet): берём ссылочное поле fld из каждой записи */
static void walk_struct_array(void* arr, int count, const char* fld, item_cb cb, void* ctx) {
    void* ac; void* ec; void* f; int esz, i, off, shift; char* data; long len;
    if (!arr) return;
    ac = A.object_get_class(arr);
    ec = A.class_get_element_class(ac);
    esz = A.class_array_element_size(ac);
    if (!ec || esz <= 0) return;
    f = find_field(ec, fld);
    if (!f) return;
    shift = A.class_is_valuetype(ec) ? struct_shift(ec) : 0;
    off = A.field_get_offset(f) - shift;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 100000) return;
    if (count < 0 || count > len) count = (int)len;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < count; i++) {
        void* it = *(void**)(data + (uptr)i * (uptr)esz + off);
        if (it) cb(it, ctx);
    }
}
static int int_field(void* o, const char* a, const char* b, const char* c, int dflt) {
    int ok = 0; double v;
    v = num_field(o, a, &ok); if (ok) return (int)v;
    if (b) { v = num_field(o, b, &ok); if (ok) return (int)v; }
    if (c) { v = num_field(o, c, &ok); if (ok) return (int)v; }
    return dflt;
}
static char g_collKind[96];
static int walk_collection(void* coll, item_cb cb, void* ctx) {
    void* k; const char* nm; void* arr;
    if (!coll) return 0;
    k = A.object_get_class(coll);
    nm = A.class_get_name(k);
    if (!seq(g_collKind, nm)) { unsigned i; for (i = 0; i < 95 && nm[i]; i++) g_collKind[i] = nm[i]; g_collKind[i] = 0; dlog2("PlayersInStream: тип ", nm); }
    if (ends_with(nm, "[]")) { walk_ref_array(coll, -1, 1, cb, ctx); return 1; }
    if (sprefix(nm, "List`1")) {
        arr = ref_field(coll, "_items"); if (!arr) arr = ref_field(coll, "items");
        walk_ref_array(arr, int_field(coll, "_size", "size", NULL, -1), 1, cb, ctx);
        return 1;
    }
    if (sprefix(nm, "Dictionary`2")) {
        arr = ref_field(coll, "_entries"); if (!arr) arr = ref_field(coll, "entries");
        if (arr) { walk_struct_array(arr, int_field(coll, "_count", "count", NULL, -1), "value", cb, ctx); return 1; }
        arr = ref_field(coll, "valueSlots");                         /* старый Mono */
        if (arr) { walk_ref_array(arr, int_field(coll, "touchedSlots", NULL, NULL, -1), 1, cb, ctx); return 1; }
    }
    if (sprefix(nm, "HashSet`1")) {
        arr = ref_field(coll, "_entries"); if (!arr) arr = ref_field(coll, "m_slots"); if (!arr) arr = ref_field(coll, "_slots");
        if (arr) { walk_struct_array(arr, int_field(coll, "_count", "m_lastIndex", "_lastIndex", -1), "value", cb, ctx); return 1; }
    }
    if (!g_fieldsDumped) dump_class(k, "PlayersInStream (неизвестный тип коллекции)");
    return 0;
}

/* ───────────── чтение игроков ───────────── */
typedef struct {
    int id; int hasPos; float x, y, z; double hp, ar, ping, score; int veh; char label[96]; char nick[64]; int labelId;
} PInfo;
#define MAXP 256
static PInfo g_pl[MAXP];
static int g_np;
static void* g_local;

static int read_pos(void* player, float* out) {
    int nu, ok = 0; const char* sname; void* sf; void* sc; char* sp; int shift, poff; void* pf;
    nu = int_field(player, "NetworkUpdate", "m_NetworkUpdateType", NULL, 1);
    sname = nu == 2 ? "m_LastDriveSyncData" : nu == 3 ? "m_LastPassengerSyncData" : "m_LastOnFootSyncData";
    sf = find_field(A.object_get_class(player), sname);
    if (!sf) return 0;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc) return 0;
    if (A.class_is_valuetype(sc)) sp = (char*)player + A.field_get_offset(sf);
    else { sp = *(char**)((char*)player + A.field_get_offset(sf)); if (!sp) return 0; shift = 0; goto have; }
    shift = struct_shift(sc);
have:
    pf = find_field(sc, "Position");
    if (!pf) return 0;
    poff = A.field_get_offset(pf) - (A.class_is_valuetype(sc) ? shift : 0);
    if (!A.class_is_valuetype(sc)) poff = A.field_get_offset(pf);
    {
        float* v = (float*)(sp + poff);
        if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return 0;
        if (v[0] > 1e6f || v[0] < -1e6f || v[1] > 1e6f || v[1] < -1e6f) return 0;
        out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; ok = 1;
    }
    return ok;
}

/* UTF-16 → UTF-8 */
static void utf16_to_utf8(const u16* s, int n, char* out, int cap) {
    int i, o = 0;
    for (i = 0; i < n && o + 4 < cap; i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xE000) { out[o++] = '?'; i++; continue; }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}
static void* g_mGetText;
static void read_label(void* player, PInfo* p) {
    void* tags = ref_field(player, "Tags");
    void* lab; void* exc = NULL; void* s;
    p->label[0] = 0; p->nick[0] = 0; p->labelId = -1;
    if (!tags) return;
    lab = ref_field(tags, "m_NickName");
    if (!lab) return;
    if (!g_mGetText) g_mGetText = find_method(A.object_get_class(lab), "get_text", 0);
    if (!g_mGetText) return;
    s = A.runtime_invoke(g_mGetText, lab, NULL, &exc);
    if (exc || !s) return;
    {
        char raw[160]; int n = A.string_length(s), i, o = 0, depth = 0;
        const u16* ch = A.string_chars(s);
        if (n <= 0 || n > 80 || !ch) return;
        utf16_to_utf8(ch, n, raw, sizeof raw);
        for (i = 0; raw[i] && o < (int)sizeof(p->label) - 1; i++) {      /* убираем rich-text теги <...> */
            if (raw[i] == '<') { depth++; continue; }
            if (raw[i] == '>' && depth) { depth--; continue; }
            if (!depth) p->label[o++] = raw[i];
        }
        p->label[o] = 0;
    }
    /* «Ник_Фамилия (123)» → ник и ID */
    {
        int len = (int)slen(p->label), i, nn = 0;
        int paren = -1;
        for (i = len - 1; i >= 0; i--) if (p->label[i] == '(') { paren = i; break; }
        if (paren > 0) {
            int j = paren + 1, v = 0, digits = 0;
            while (p->label[j] >= '0' && p->label[j] <= '9') { v = v * 10 + (p->label[j] - '0'); j++; digits++; }
            if (digits && p->label[j] == ')') p->labelId = v;
            for (i = 0; i < paren && nn < (int)sizeof(p->nick) - 1; i++) p->nick[nn++] = p->label[i];
            while (nn > 0 && p->nick[nn - 1] == ' ') nn--;
            p->nick[nn] = 0;
        } else {
            for (i = 0; i < len && nn < (int)sizeof(p->nick) - 1; i++) p->nick[nn++] = p->label[i];
            p->nick[nn] = 0;
        }
    }
}

static void on_player(void* item, void* ctx) {
    PInfo* p; float pos[3]; int ok;
    (void)ctx;
    if (g_np >= MAXP || !item) return;
    p = &g_pl[g_np];
    p->id = int_field(item, "m_ID", NULL, NULL, -1);
    if (item == g_local) return;
    p->hasPos = read_pos(item, pos);
    p->x = p->hasPos ? pos[0] : 0; p->y = p->hasPos ? pos[1] : 0; p->z = p->hasPos ? pos[2] : 0;
    p->hp = num_field(item, "m_Health", &ok);
    p->ar = num_field(item, "m_Armour", &ok);
    p->ping = num_field(item, "Ping", &ok);
    p->score = num_field(item, "Score", &ok);
    p->veh = ref_field(item, "Vehicle") ? 1 : 0;
    read_label(item, p);
    if (p->id < 0 && p->labelId >= 0) p->id = p->labelId;
    g_np++;
}

/* ───────────── итоговый JSON ───────────── */
static char g_json[2][32768];
static volatile int g_jsonCur;
static volatile unsigned g_ticks;
static char g_stage[160] = "старт";

static void set_stage(const char* s) {
    unsigned i = 0;
    if (seq(g_stage, s)) return;
    for (; s[i] && i < sizeof g_stage - 1; i++) g_stage[i] = s[i];
    g_stage[i] = 0;
    dlog2("стадия: ", s);
}
static void publish(int ok, const char* why, int havePos, float mx, float my, float mz, int meId) {
    Buf b; int next = g_jsonCur ^ 1, i;
    b_init(&b, g_json[next], sizeof g_json[next]);
    b_s(&b, "{\"v\":1,\"ok\":"); b_int(&b, ok);
    b_s(&b, ",\"stage\":"); b_jstr(&b, why);
    b_s(&b, ",\"tick\":"); b_int(&b, (long)g_ticks);
    b_s(&b, ",\"me\":{\"id\":"); b_int(&b, meId);
    if (havePos) { b_s(&b, ",\"x\":"); b_fix(&b, mx, 1); b_s(&b, ",\"y\":"); b_fix(&b, my, 1); b_s(&b, ",\"z\":"); b_fix(&b, mz, 1); }
    b_s(&b, "},\"n\":[");
    for (i = 0; i < g_np; i++) {
        PInfo* p = &g_pl[i];
        if (i) b_ch(&b, ',');
        b_s(&b, "{\"id\":"); b_int(&b, p->id);
        b_s(&b, ",\"nick\":"); b_jstr(&b, p->nick);
        b_s(&b, ",\"label\":"); b_jstr(&b, p->label);
        if (p->hasPos) {
            b_s(&b, ",\"x\":"); b_fix(&b, p->x, 1); b_s(&b, ",\"y\":"); b_fix(&b, p->y, 1); b_s(&b, ",\"z\":"); b_fix(&b, p->z, 1);
            if (havePos) {
                double dx = p->x - mx, dy = p->y - my, dz = p->z - mz;
                b_s(&b, ",\"d\":"); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 1);
                b_s(&b, ",\"d2\":"); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy), 1);
            }
        }
        b_s(&b, ",\"hp\":"); b_fix(&b, p->hp, 0);
        b_s(&b, ",\"ar\":"); b_fix(&b, p->ar, 0);
        b_s(&b, ",\"ping\":"); b_fix(&b, p->ping, 0);
        b_s(&b, ",\"score\":"); b_fix(&b, p->score, 0);
        b_s(&b, ",\"veh\":"); b_int(&b, p->veh);
        b_ch(&b, '}');
    }
    b_s(&b, "]}");
    g_jsonCur = next;
}

static void collect_tick(void) {
    void* pool; void* stream; float me[3] = {0, 0, 0}; int havePos = 0, meId = -1;
    g_ticks++;
    if (!il2cpp_open()) { set_stage("ждём libil2cpp"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_domain) g_domain = A.domain_get();
    if (!g_domain) { set_stage("ждём домен IL2CPP"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_img) g_img = find_image();
    if (!g_img) { set_stage("ждём Assembly-CSharp"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_cPlayerPool) {
        g_cPlayerPool = A.class_from_name(g_img, "Hassle.Global.Pooling", "PlayerPool");
        g_cPools = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
        if (!g_cPlayerPool) { set_stage("класс PlayerPool не найден"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    }
    if (!tl_attached) { ensure_attached(); dlog("поток присоединён к IL2CPP"); }
    if (!g_poolSrcField) scan_for_pool();
    if (!g_poolSrcField) { set_stage("static-поле с PlayerPool не найдено"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    pool = current_pool();
    if (!pool) { set_stage("PlayerPool ещё не создан (вы не в игре?)"); g_np = 0; publish(0, g_stage, 0, 0, 0, 0, -1); return; }

    if (!g_fieldsDumped) {
        dump_class(g_cPlayerPool, "PlayerPool");
        g_fieldsDumped = 1;
    }
    g_local = ref_field(pool, "LocalPlayer");
    if (g_local) {
        meId = int_field(g_local, "m_ID", NULL, NULL, -1);
        havePos = read_pos(g_local, me);
    }
    stream = ref_field(pool, "PlayersInStream");
    if (!stream) { set_stage("PlayersInStream = null"); g_np = 0; publish(0, g_stage, havePos, me[0], me[1], me[2], meId); return; }
    g_np = 0;
    if (!walk_collection(stream, on_player, NULL)) { set_stage("неизвестный тип PlayersInStream — см. /diag"); publish(0, g_stage, havePos, me[0], me[1], me[2], meId); return; }
    set_stage("работает");
    {   /* один раз покажем поля первого игрока — нужно, чтобы поправить код без пересборки догадок */
        static int once;
        if (!once && g_np > 0) {
            /* повторный обход, только чтобы получить объект */
            once = 1;
        }
    }
    publish(1, g_stage, havePos, me[0], me[1], me[2], meId);
}

static void init_log(void) {
    void* lg;
    if (g_alog) return;
    lg = dlopen("liblog.so", RTLD_NOW_);
    if (lg) *(void**)&g_alog = dlsym(lg, "__android_log_write");
}

/* Выключатель для диагностики: если существует файл near_off, мод ничего не запускает (работает только подмена оригинала).
   /sdcard/Android/data/<пакет>/files/near_off  или  /data/local/tmp/near_off */
static int off_switch(void) {
    char cmd[128], path[256]; Buf b; int fd; long n, i;
    fd = open("/data/local/tmp/near_off", 0);
    if (fd >= 0) { close(fd); return 1; }
    fd = open("/proc/self/cmdline", 0);
    if (fd < 0) return 0;
    n = read(fd, cmd, sizeof cmd - 1); close(fd);
    if (n <= 0) return 0;
    cmd[n] = 0;
    for (i = 0; cmd[i]; i++) if (cmd[i] == ':') { cmd[i] = 0; break; }
    b_init(&b, path, sizeof path);
    b_s(&b, "/sdcard/Android/data/"); b_s(&b, cmd); b_s(&b, "/files/near_off");
    fd = open(path, 0);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

static void try_register_cmd(void);

static void* collector_main(void* arg) {
    int w;
    (void)arg;
    init_log();
    dlog("collector: старт, жду готовности игры (без фиксированной паузы)");
    set_stage("ждём libil2cpp / Assembly-CSharp");
    while (!il2cpp_light()) { publish(0, g_stage, 0, 0, 0, 0, -1); usleep(POLL_US); }
    dlog("collector: IL2CPP виден, короткая стабилизация");
    usleep(SETTLE_MS * 1000);
    set_stage("ждём запуск игры (Game.Instance)");
    while (!game_ready()) { publish(0, g_stage, 0, 0, 0, 0, -1); usleep(POLL_US); }
    dlog("collector: игра готова (Game.Instance создан), начинаю");
    for (w = 0; w < NEAR_EXTRA_DELAY_S; w++) usleep(1000000);
    for (;;) {
        try_register_cmd();                              /* нативная /rec: один раз, как только в реестре появится образец делегата */
        collect_tick();
        usleep(TICK_US);
    }
    return NULL;
}

/* ───────────── HTTP-сервер на loopback ───────────── */
struct sa4 { u16 family; u16 port; u32 addr; u8 zero[8]; };
struct tv { long sec; long usec; };

static int send_all(int fd, const char* p, unsigned n) {
    while (n) {
        long w = send(fd, p, n, 0x4000);               /* MSG_NOSIGNAL */
        if (w <= 0) return 0;
        p += w; n -= (unsigned)w;
    }
    return 1;
}
static void respond(int fd, const char* status, const char* ctype, const char* body, unsigned blen) {
    char h[320]; Buf b;
    b_init(&b, h, sizeof h);
    b_s(&b, "HTTP/1.1 "); b_s(&b, status);
    b_s(&b, "\r\nContent-Type: "); b_s(&b, ctype);
    b_s(&b, "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ");
    b_int(&b, (long)blen);
    b_s(&b, "\r\n\r\n");
    if (send_all(fd, h, b.n) && blen) send_all(fd, body, blen);
}

static char g_diag_out[24576 + 1024];

/* ───────────── реконнект: GET /rec?t=N ─────────────
   Повторяет Hassle::Commands::Reconnect оригинального libHassleExtended (/rec): берёт Game.Network (static), читает
   m_Host/m_Port/m_Password/m_NickName, вызывает OnApplicationQuit() (посылает ID_DISCONNECTION_NOTIFICATION, если State == 2)
   и DelayedConnect(host, port, password, nick, секунды) — внутри UniTask.Delay(N) и затем NetworkClient.Connect(...). */
static long g_recUntil;                 /* до какого времени (time()) предыдущий реконнект ещё «в работе» */
static char g_rec_out[1024];

static int query_int(const char* rq, const char* key, int dflt) {
    const char* q; unsigned kl = slen(key);
    for (q = rq; *q && *q != '\r' && *q != '\n'; q++) {
        if ((*q == '?' || *q == '&') && sprefix(q + 1, key) && q[1 + kl] == '=') {
            int v = 0, d = 0; const char* p = q + 2 + kl;
            while (*p >= '0' && *p <= '9' && v < 100000) { v = v * 10 + (*p - '0'); p++; d++; }
            return d ? v : dflt;
        }
    }
    return dflt;
}
static void exc_text(void* exc, char* out, int cap) {
    void* m; void* e2 = NULL; void* s;
    out[0] = 0;
    if (!exc) return;
    m = find_method(A.object_get_class(exc), "ToString", 0);
    if (!m) return;
    s = A.runtime_invoke(m, exc, NULL, &e2);
    if (e2 || !s) return;
    { int n = A.string_length(s); if (n > 240) n = 240; utf16_to_utf8(A.string_chars(s), n, out, cap); }
}
typedef struct { int ok; int t; int state; int port; int quit; char why[200]; char exc[256]; } RecRes;
static void rec_set(RecRes* r, const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof r->why - 1; i++) r->why[i] = why[i];
    r->why[i] = 0; r->ok = 0;
    dlog2("реконнект: отказ: ", r->why);
}

/* Общая часть: и для HTTP /rec, и для нативной чат-команды. IL2CPP при необходимости поднимается ЛЕНИВО — набрать /rec можно только
   тогда, когда игра уже идёт, поэтому ждать сборщика (бывшие 60 секунд) не нужно. */
static void do_reconnect(int t, RecRes* r) {
    int state = -1, port = 0, ok = 0;
    void *fn, *nc = NULL, *nk, *mq, *md, *exc = NULL, *host, *pw, *nick;
    unsigned short port16; float tf; char et[256]; long now;

    r->ok = 0; r->state = -1; r->port = 0; r->quit = 0; r->why[0] = 0; r->exc[0] = 0;
    if (t < 0) t = 0;
    if (t > 600) t = 600;
    r->t = t;
    if (!il2cpp_light()) { rec_set(r, "IL2CPP ещё не готов (игра грузится, подождите)"); return; }
    now = time(NULL);
    if (now < g_recUntil) { rec_set(r, "реконнект уже запущен, подождите"); return; }
    ensure_attached();

    if (!g_cGame) g_cGame = A.class_from_name(g_img, "Hassle", "Game");
    if (!g_cGame) { rec_set(r, "класс Hassle.Game не найден"); return; }
    fn = A.class_get_field_from_name(g_cGame, "Network");
    if (!fn) { rec_set(r, "поле Game.Network не найдено"); return; }
    A.field_static_get_value(fn, &nc);
    if (!nc) { rec_set(r, "Game.Network = null (вы не на сервере?)"); return; }
    nk = A.object_get_class(nc);

    state = (int)num_field(nc, "State", &ok);
    host = ref_field(nc, "m_Host");
    pw = ref_field(nc, "m_Password");
    nick = ref_field(nc, "m_NickName");
    port = (int)num_field(nc, "m_Port", &ok);
    if (!host || !nick) { rec_set(r, "нет данных подключения (m_Host/m_NickName пусты)"); return; }
    mq = find_method(nk, "OnApplicationQuit", 0);
    md = find_method(nk, "DelayedConnect", 5);
    if (!mq || !md) { rec_set(r, !md ? "метод DelayedConnect не найден" : "метод OnApplicationQuit не найден"); return; }

    dlog_i("реконнект: старт, секунд ", t);
    dlog_i("реконнект: State ", state);
    dlog_i("реконнект: порт ", port);

    /* 1) корректный выход с сервера (внутри сам проверяет State == Connected) */
    exc = NULL;
    A.runtime_invoke(mq, nc, NULL, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: OnApplicationQuit бросил: ", et); }
    else r->quit = 1;

    /* 2) подключение через N секунд */
    port16 = (unsigned short)port; tf = (float)t;
    {
        void* args[5];
        args[0] = host; args[1] = &port16; args[2] = pw; args[3] = nick; args[4] = &tf;
        exc = NULL;
        A.runtime_invoke(md, nc, args, &exc);
        if (exc && t == 0) {   /* на случай, если нулевая задержка не принимается: повторяем с минимальной (0.1 с) */
            exc_text(exc, et, sizeof et);
            dlog2("реконнект: DelayedConnect(0) бросил, повтор с 0.1 с: ", et);
            tf = 0.1f; exc = NULL;
            A.runtime_invoke(md, nc, args, &exc);
        }
    }
    if (exc) {
        exc_text(exc, et, sizeof et);
        dlog2("реконнект: DelayedConnect бросил: ", et);
        rec_set(r, "DelayedConnect: исключение");
        { unsigned i = 0; for (; et[i] && i < sizeof r->exc - 1; i++) r->exc[i] = et[i]; r->exc[i] = 0; }
        return;
    }
    g_recUntil = now + t + 3;
    r->ok = 1; r->state = state; r->port = port;
    dlog("реконнект: DelayedConnect запущен");
}

static void handle_rec(int c, const char* rq) {
    RecRes r; Buf b;
    do_reconnect(query_int(rq, "t", 0), &r);
    b_init(&b, g_rec_out, sizeof g_rec_out);
    if (r.ok) {
        b_s(&b, "{\"ok\":1,\"t\":"); b_int(&b, r.t);
        b_s(&b, ",\"state\":"); b_int(&b, r.state);
        b_s(&b, ",\"port\":"); b_int(&b, r.port);
        b_s(&b, ",\"quit\":"); b_int(&b, r.quit);
        b_ch(&b, '}');
    } else {
        b_s(&b, "{\"ok\":0,\"why\":"); b_jstr(&b, r.why);
        if (r.exc[0]) { b_s(&b, ",\"exc\":"); b_jstr(&b, r.exc); }
        b_ch(&b, '}');
    }
    respond(c, "200 OK", "application/json", g_rec_out, b.n);
}

/* ───────────── нативная чат-команда /rec (без rec.js и без портов) ─────────────
   В игре есть реестр чат-команд: WebViewSystem.m_ChatCommandEvents (Dictionary<string, ChatCommandEvent>), его заполняет сама игра через
   RegisterChatCommandEvent; WebViewSystem.SendChatInput(текст) ищет команду там. Мы берём любой уже зарегистрированный игрой делегат
   (с целью-объектом), клонируем его и подменяем method_ptr на нашу функцию — IL2CPP вызовет её как (this, string[] args, MethodInfo*).
   Команда выполняется в главном потоке Unity. Ключи кладём и «/rec», и «rec» (какой формат ждёт реестр — выясняется по журналу /diag). */
static int g_cmdDone, g_cmdTries, g_keyLogged;
static void* g_tplDelegate;
static int g_dictCount, g_keyHasSlash;

typedef void (*kv_cb)(void* key, void* val, void* ctx);
static void dict_each(void* dict, kv_cb cb, void* ctx) {
    void *arr, *ac, *ec, *fk, *fv, *fh; int esz, i, shift, cnt, offk, offv, offh = 0; char* data; long len;
    if (!dict) return;
    arr = ref_field(dict, "_entries"); if (!arr) arr = ref_field(dict, "entries");
    if (!arr) return;
    ac = A.object_get_class(arr); ec = A.class_get_element_class(ac); esz = A.class_array_element_size(ac);
    if (!ec || esz <= 0) return;
    fk = find_field(ec, "key"); fv = find_field(ec, "value"); fh = find_field(ec, "hashCode");
    if (!fk || !fv) return;
    shift = A.class_is_valuetype(ec) ? struct_shift(ec) : 0;
    offk = A.field_get_offset(fk) - shift; offv = A.field_get_offset(fv) - shift;
    if (fh) offh = A.field_get_offset(fh) - shift;
    cnt = int_field(dict, "_count", "count", NULL, -1);
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 100000) return;
    if (cnt < 0 || cnt > len) cnt = (int)len;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < cnt; i++) {
        char* e = data + (uptr)i * (uptr)esz;
        if (fh && *(int*)(e + offh) < 0) continue;        /* свободная запись словаря */
        cb(*(void**)(e + offk), *(void**)(e + offv), ctx);
    }
}
static void tpl_cb(void* key, void* val, void* ctx) {
    (void)ctx;
    g_dictCount++;
    if (key) {
        int n = A.string_length(key); const u16* ch = A.string_chars(key);
        if (n > 0 && ch && ch[0] == '/') g_keyHasSlash = 1;
        if (g_keyLogged < 40 && n > 0 && n < 60 && ch) {
            char tmp[160]; utf16_to_utf8(ch, n, tmp, sizeof tmp);
            dlog2("реестр команд: ключ ", tmp); g_keyLogged++;
        }
    }
    if (val && !g_tplDelegate) {
        void* tgt = ref_field(val, "m_target"); void* dels = ref_field(val, "delegates");
        if (tgt && !dels) g_tplDelegate = val;           /* обычный (не multicast) делегат с целью — годится как образец */
    }
}

static void chat_say(const char* msg) {
    void *cw, *m, *exc = NULL, *args[2];
    if (!A.string_new || !g_img) return;
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    if (!cw) return;
    m = A.class_get_method_from_name(cw, "AddChatMessage", 2);
    if (!m) return;
    args[0] = A.string_new(msg); args[1] = A.string_new("white");
    A.runtime_invoke(m, NULL, args, &exc);
}

/* аргументы команды: string[]; число секунд — первый числовой элемент (не важно, входит ли сама «/rec» в массив); «diag» — диагностика */
static int parse_cmd_args(void* arr, int* diag) {
    long len; int i, t = -1; char* data;
    *diag = 0;
    if (!arr) return -1;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return -1;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[64]; int n; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 40 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        if (seq(tmp, "diag")) { *diag = 1; continue; }
        if (t < 0 && tmp[0] >= '0' && tmp[0] <= '9') {
            int v = 0, k = 0;
            while (tmp[k] >= '0' && tmp[k] <= '9' && v < 100000) { v = v * 10 + (tmp[k] - '0'); k++; }
            t = v;
        }
    }
    return t;
}

static int g_port;
/* Вызывается игрой в главном потоке: (this-цель делегата, string[] args, MethodInfo*) */
static void native_rec_cmd(void* self, void* args, void* mi) {
    RecRes r; int diag = 0, t; Buf b; char msg[300];
    (void)self; (void)mi;
    t = parse_cmd_args(args, &diag);
    dlog_i("нативная /rec: вызвана, секунд ", t);
    b_init(&b, msg, sizeof msg);
    if (diag) {
        b_s(&b, "Near: стадия «"); b_s(&b, g_stage); b_s(&b, "», HTTP-порт "); b_int(&b, g_port);
        b_s(&b, ", /rec нативно: "); b_s(&b, g_cmdDone == 1 ? "да" : "нет");
        chat_say(msg);
        return;
    }
    if (t < 0) t = 5;                                   /* как в rec.js: по умолчанию 5 секунд */
    do_reconnect(t, &r);
    if (r.ok) { b_s(&b, "Реконнект: отключено, подключение через "); b_int(&b, r.t); b_s(&b, " сек"); }
    else { b_s(&b, "Реконнект: "); b_s(&b, r.why); }
    chat_say(msg);
}

static void cmd_fail(const char* why) { g_cmdDone = -1; dlog2("нативная /rec: не получилось, остаётся HTTP+rec.js. Причина: ", why); }
static int cmd_put(void* dict, void* setitem, const char* key, void* del) {
    void* args[2]; void* exc = NULL; char et[256];
    args[0] = A.string_new(key); args[1] = del;
    A.runtime_invoke(setitem, dict, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("нативная /rec: set_Item бросил: ", et); return 0; }
    return 1;
}
static void try_register_cmd(void) {
    void *cw, *fd, *dict = NULL, *kls, *nd, *mset, *f_mp, *f_ii; int size, i, off_mp, off_ii;
    uptr mp, ii;
    if (g_cmdDone) return;
    if (++g_cmdTries > 1500) { cmd_fail("реестр команд так и не заполнился (нет образца делегата)"); return; }
    if (!A.string_new || !A.object_new || !A.class_instance_size) { cmd_fail("нет il2cpp_string_new / object_new / class_instance_size"); return; }
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    if (!cw) return;
    fd = A.class_get_field_from_name(cw, "m_ChatCommandEvents");
    if (!fd) { cmd_fail("поле WebViewSystem.m_ChatCommandEvents не найдено"); return; }
    A.field_static_get_value(fd, &dict);
    if (!dict) return;                                   /* реестр ещё не создан */
    g_dictCount = 0; g_tplDelegate = NULL; g_keyHasSlash = 0;
    dict_each(dict, tpl_cb, NULL);
    if (!g_tplDelegate) return;                          /* игра ещё не зарегистрировала ни одной команды — образца нет, ждём */

    kls = A.object_get_class(g_tplDelegate);
    size = A.class_instance_size(kls);
    if (size < 0x40 || size > 0x200) { cmd_fail("странный размер делегата"); return; }
    f_mp = find_field(kls, "method_ptr");
    if (!f_mp) { cmd_fail("в делегате нет поля method_ptr"); return; }
    off_mp = A.field_get_offset(f_mp);
    f_ii = find_field(kls, "invoke_impl");
    off_ii = f_ii ? A.field_get_offset(f_ii) : 0;
    mset = find_method(A.object_get_class(dict), "set_Item", 2);
    if (!mset) { cmd_fail("у словаря нет set_Item"); return; }

    nd = A.object_new(kls);
    if (!nd) { cmd_fail("object_new вернул null"); return; }
    for (i = 2 * (int)sizeof(void*); i < size; i++) ((char*)nd)[i] = ((char*)g_tplDelegate)[i];
    mp = *(uptr*)((char*)g_tplDelegate + off_mp);
    *(uptr*)((char*)nd + off_mp) = (uptr)native_rec_cmd;
    if (f_ii) {
        ii = *(uptr*)((char*)g_tplDelegate + off_ii);
        if (ii == mp) *(uptr*)((char*)nd + off_ii) = (uptr)native_rec_cmd;   /* invoke_impl совпадал с method_ptr — подменяем тоже */
    }
    dlog_i("нативная /rec: записей в реестре ", g_dictCount);
    dlog_i("нативная /rec: ключи начинаются с «/» ", g_keyHasSlash);
    if (cmd_put(dict, mset, "/rec", nd) & cmd_put(dict, mset, "rec", nd)) {
        g_cmdDone = 1;
        dlog("нативная /rec: зарегистрирована («/rec» и «rec»)");
    } else cmd_fail("не удалось записать делегат в реестр");
}


static void handle_diag(int c) {
    Buf b;
    b_init(&b, g_diag_out, sizeof g_diag_out);
    if (g_diag_ready) { unsigned i; for (i = 0; i < (unsigned)g_diag.n && b.n + 1 < b.cap; i++) b_ch(&b, g_diag_mem[i]); }
    respond(c, "200 OK", "text/plain; charset=utf-8", g_diag_out, b.n);
}

static void* server_main(void* arg) {
    int fd = -1, i, one = 1;
    (void)arg;
    for (;;) {
        for (i = 0; i < NEAR_PORT_COUNT && fd < 0; i++) {
            struct sa4 sa; int s = socket(2, 1, 0);
            if (s < 0) break;
            setsockopt(s, 1, 2, &one, sizeof one);          /* SOL_SOCKET, SO_REUSEADDR */
            sa.family = 2; sa.port = (u16)(((NEAR_PORT_FIRST + i) >> 8) | (((NEAR_PORT_FIRST + i) & 255) << 8));
            sa.addr = 0x0100007F;                           /* 127.0.0.1 */
            { int z; for (z = 0; z < 8; z++) sa.zero[z] = 0; }
            if (bind(s, &sa, sizeof sa) == 0 && listen(s, 8) == 0) { fd = s; g_port = NEAR_PORT_FIRST + i; }
            else close(s);
        }
        if (fd < 0) { usleep(3000000); continue; }
        dlog_i("HTTP: слушаю порт ", g_port);
        for (;;) {
            int c = accept(fd, NULL, NULL);
            char rq[512]; long n; struct tv t;
            if (c < 0) { usleep(100000); continue; }
            t.sec = 2; t.usec = 0;
            setsockopt(c, 1, 20, &t, sizeof t);             /* SO_RCVTIMEO */
            setsockopt(c, 1, 21, &t, sizeof t);             /* SO_SNDTIMEO */
            n = recv(c, rq, sizeof rq - 1, 0);
            if (n > 0) {
                rq[n] = 0;
                if (sprefix(rq, "OPTIONS ")) respond(c, "204 No Content", "text/plain", "", 0);
                else if (sprefix(rq, "GET /near")) { const char* j = g_json[g_jsonCur]; respond(c, "200 OK", "application/json", j, slen(j)); }
                else if (sprefix(rq, "GET /rec")) handle_rec(c, rq);
                else if (sprefix(rq, "GET /diag")) handle_diag(c);
                else if (sprefix(rq, "GET /ping")) respond(c, "200 OK", "text/plain", "hassle-near", 11);
                else respond(c, "404 Not Found", "text/plain", "no", 2);
            }
            close(c);
        }
    }
    return NULL;
}

/* ───────────── запуск ───────────── */
static int g_started;
static void start_threads(void) {
    unsigned long t1 = 0, t2 = 0;
    if (g_started) return;
    g_started = 1;
    init_log();
    if (off_switch()) { dlog("near_off найден: мод отключён, работает только оригинал"); return; }
    dlog("start_threads: near_off нет, мод включён");
    dlog("start_threads: запускаю сервер и сборщик");
    pthread_create(&t1, NULL, server_main, NULL);
    pthread_create(&t2, NULL, collector_main, NULL);
}

#ifndef NEAR_TEST
__attribute__((constructor)) static void near_ctor(void) {
    init_log();
    dlog("near_ctor: наша .so загружена системой (ещё до JNI_OnLoad)");
}
typedef int (*jni_onload_t)(void*, void*);
typedef void (*jni_onunload_t)(void*, void*);
static void* g_orig;

/* Оригинал лежит рядом: libHassleExtend2.so (старый клиент) или libHassleMain2.so (чистый APK, подмена libmain.so) */
EXPORT int JNI_OnLoad(void* vm, void* reserved) {
    int ver = 0x00010006;                                  /* JNI_VERSION_1_6 */
    init_log();
    dlog_i("JNI_OnLoad: вход, разрядность указателя (бит) ", (long)(sizeof(void*) * 8));
    dlog("JNI_OnLoad: грузим оригинал (libHassleExtend2.so / libHassleMain2.so)");
    g_orig = dlopen("libHassleExtend2.so", RTLD_NOW_);
    if (g_orig) dlog("JNI_OnLoad: режим старого клиента (libHassleExtend2.so)");
    else {
        g_orig = dlopen("libHassleMain2.so", RTLD_NOW_);
        if (g_orig) dlog("JNI_OnLoad: режим чистого APK (libHassleMain2.so = оригинальный libmain.so)");
    }
    if (g_orig) {
        jni_onload_t f = (jni_onload_t)dlsym(g_orig, "JNI_OnLoad");
        dlog("JNI_OnLoad: оригинал загружен, вызываю его JNI_OnLoad");
        if (f) ver = f(vm, reserved);
        dlog_i("JNI_OnLoad: оригинал вернул версию ", ver);
    } else {
        const char* de = dlerror();
        dlog("JNI_OnLoad: оригинал (libHassleExtend2.so / libHassleMain2.so) НЕ загрузился");
        dlog2("JNI_OnLoad: dlerror: ", de ? de : "(нет)");
    }
    start_threads();
    dlog("JNI_OnLoad: выход");
    return ver;
}
EXPORT void JNI_OnUnload(void* vm, void* reserved) {
    if (g_orig) {
        jni_onunload_t f = (jni_onunload_t)dlsym(g_orig, "JNI_OnUnload");
        if (f) f(vm, reserved);
    }
}
#else
void near_test_start(void) { start_threads(); }
void near_test_tick(void) { collect_tick(); }
const char* near_test_json(void) { return g_json[g_jsonCur]; }
const char* near_test_diag(void) { return g_diag_mem; }
int near_test_port(void) { return g_port; }
#endif
