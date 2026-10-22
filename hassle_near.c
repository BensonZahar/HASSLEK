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
 *   • Реконнект — только нативная чат-команда /rec [секунд 0–600] (HTTP-эндпоинта /rec больше нет), как в оригинальном libHassleExtended:
 *     Game.Network.OnApplicationQuit() (шлёт серверу ID_DISCONNECTION_NOTIFICATION, если State == Connected), затем
 *     Game.Network.DelayedConnect(m_Host, m_Port, m_Password, m_NickName, N). Game.Reset() сознательно не вызывается.
 *
 * Версия без паузы и без JS для /rec:
 *   • Вместо слепых 60 секунд сборщик ждёт РЕАЛЬНУЮ готовность игры: libil2cpp загружена → виден Assembly-CSharp →
 *     (короткая стабилизация SETTLE_MS) → создан Hassle.Game.Instance. Пока ждёт, IL2CPP не трогает дальше лёгкого опроса.
 *   • /rec работает и без сборщика: команда сама лениво инициализирует IL2CPP (набрать её можно только когда игра уже идёт).
 *   • Нативная чат-команда: мод регистрирует «/rec» прямо в реестре команд игры (WebViewSystem.m_ChatCommandEvents), так что
 *     ни rec.js, ни HTTP-порты для /rec не нужны. Делегат клонируется с уже зарегистрированной игрой команды, подменяется method_ptr.
 *
 * /nocol — отключить столкновения со всеми игроками и NPC (только на вашем клиенте, сервер ничего не знает).
 *   Игроки — PlayerPool.PlayersInStream, NPC — Pools.Actors (RPC Hassle.Global.NPC создаёт Actor'ов). Берём капсулу каждого
 *   (Character.Movement/SimpleMovement → _capsuleCollider / m_Collider) и в главном потоке зовём у вашего движения
 *   IgnoreCollision(Collider, true) + Physics.IgnoreCollision. По слою нельзя: у игры нет слоя «персонажи» (капсулы на Default вместе с миром).
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
#ifndef NEAR_SETTLE_S
#define NEAR_SETTLE_S 12      /* (пробовали 1 и 3 — иногда вылетало на старте, вернули 12) */
/* сколько секунд после появления libil2cpp.so мод НЕ вызывает ни одной её функции (идёт il2cpp_init). Можно менять файлом HLOG/near_delay.txt (число секунд) без пересборки */
#endif
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
    /* необязательные (нужны только для /nocol) */
    void* (*class_get_methods)(void*, void**);
    const char* (*method_get_name)(void*);
    unsigned (*method_get_param_count)(void*);
    void* (*method_get_param)(void*, unsigned);
} A;

static void* g_il2cpp;
static void* g_domain;
static volatile int g_safe;          /* 1 — период ожидания после загрузки libil2cpp прошёл, функции IL2CPP звать можно */

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
    *(void**)&A.class_get_methods = dlsym(g_il2cpp, "il2cpp_class_get_methods");
    *(void**)&A.method_get_name = dlsym(g_il2cpp, "il2cpp_method_get_name");
    *(void**)&A.method_get_param_count = dlsym(g_il2cpp, "il2cpp_method_get_param_count");
    *(void**)&A.method_get_param = dlsym(g_il2cpp, "il2cpp_method_get_param");
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
/* libil2cpp.so уже загружена игрой? Смотрим /proc/self/maps — это обычное чтение файла, с динамическим загрузчиком не связано.
   (dlopen(..., RTLD_NOLOAD) из фонового потока, пока главный поток грузит библиотеки, ронял игру — журнал оборвался на этом цикле.)
   Возврат: 1 — есть, 0 — нет, -1 — maps прочитать нельзя. */
static int lib_mapped(void) {
    static char buf[8192];
    int fd, found = 0, keep = 0, i; long n; const char* pat = "libil2cpp.so";
    fd = open("/proc/self/maps", 0);
    if (fd < 0) return -1;
    for (;;) {
        n = read(fd, buf + keep, sizeof buf - 1 - keep);
        if (n <= 0) break;
        n += keep; buf[n] = 0;
        for (i = 0; i < n; i++) if (buf[i] == 'l' && sprefix(buf + i, pat)) { found = 1; break; }
        if (found) break;
        keep = 11 < n ? 11 : (int)n;                      /* хвост, чтобы не потерять совпадение на границе блоков */
        for (i = 0; i < keep; i++) buf[i] = buf[n - keep + i];
    }
    close(fd);
    return found;
}
/* Лёгкая проверка: libil2cpp загружена, домен создан, Assembly-CSharp виден. Классов не трогает, поток не присоединяет.
   Вызывать только после g_safe: ранние вызовы il2cpp_domain_get/…_get_assemblies (до конца il2cpp_init) роняют игру.
   Шаги пишутся в журнал ДО вызова — если игра всё же упадёт, последняя строка покажет, на каком вызове. */
static int il2cpp_light(void) {
    static int l1, l2, l3;
    if (!g_safe) return 0;
    if (!g_il2cpp && !l1) { l1 = 1; dlog("probe: il2cpp_open (dlsym функций API)"); }
    if (!il2cpp_open()) return 0;
    if (!g_domain) {
        if (!l2) { l2 = 1; dlog("probe: il2cpp_domain_get"); }
        g_domain = A.domain_get();
        if (g_domain) dlog("probe: домен получен");
    }
    if (!g_domain) return 0;
    if (!g_img) {
        if (!l3) { l3 = 1; dlog("probe: il2cpp_domain_get_assemblies + поиск Assembly-CSharp"); }
        g_img = find_image();
        if (g_img) dlog("probe: Assembly-CSharp найден");
    }
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
/* Pools.Actors (ActorPool : BasePool<Actor>) — сюда RPC Hassle.Global.NPC.Create кладёт NPC. Поле может быть static или лежать в экземпляре Pools. */
static void* current_actor_pool(void) {
    void* f; void* v = NULL;
    if (!g_cPools) return NULL;
    f = A.class_get_field_from_name(g_cPools, "Actors");
    if (!f) return NULL;
    if (A.field_get_flags(f) & 0x10) { A.field_static_get_value(f, &v); return v; }
    if (g_poolSrcField && g_poolSrcIsPools) { A.field_static_get_value(g_poolSrcField, &v); return ref_field(v, "Actors"); }
    return NULL;
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
static int walk_collection_x(void* coll, item_cb cb, void* ctx, int quiet) {
    void* k; const char* nm; void* arr;
    if (!coll) return 0;
    k = A.object_get_class(coll);
    nm = A.class_get_name(k);
    if (!quiet && !seq(g_collKind, nm)) { unsigned i; for (i = 0; i < 95 && nm[i]; i++) g_collKind[i] = nm[i]; g_collKind[i] = 0; dlog2("PlayersInStream: тип ", nm); }
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
    if (!quiet && !g_fieldsDumped) dump_class(k, "PlayersInStream (неизвестный тип коллекции)");
    return 0;
}
static int walk_collection(void* coll, item_cb cb, void* ctx) { return walk_collection_x(coll, cb, ctx, 0); }

/* ───────────── чтение игроков ───────────── */
typedef struct {
    int id; int hasPos; float x, y, z; double hp, ar, ping, score; int veh; int src; char label[96]; char nick[64]; int labelId;
} PInfo;
#define MAXP 256
static PInfo g_pl[MAXP];
static int g_np;
static void* g_local;

/* Какой из трёх «последних пакетов синхронизации» актуален: 1 — пешком, 2 — водитель, 3 — пассажир.
   БЫЛА ОШИБКА: раньше тут читалось поле «NetworkUpdate», но у Player это bool («обновление по сети включено»), а read_num bool не знает
   и возвращал 0 при ok=1 — до «m_NetworkUpdateType» дело не доходило, и ВСЕГДА брался пакет «пешком». У сидящих в машине (и у вас самих
   за рулём/пассажиром) позиция застревала в точке, где они сели, — отсюда «ближе тот, кто стоит, хотя я рядом с другим».
   Теперь: у RemotePlayer — его собственный m_NetworkUpdateType (enum int32: None 0, OnFoot 1, Driver 2, Passenger 3);
   у LocalPlayer такого поля нет — смотрим Vehicle и get_IsDriver(). */
static int g_lastKind, g_meKind;
static int sync_kind(void* player) {
    void* k = A.object_get_class(player); void* f; void* m;
    f = find_field(k, "m_NetworkUpdateType");
    if (f) {
        int v = *(int*)((char*)player + A.field_get_offset(f));
        if (v >= 1 && v <= 3) return v;
    }
    if (!ref_field(player, "Vehicle")) return 1;
    m = find_method(k, "get_IsDriver", 0);
    if (m) {
        void* exc = NULL; void* r = A.runtime_invoke(m, player, NULL, &exc);
        if (!exc && r) return *((u8*)r + HDR) ? 2 : 3;
    }
    return 2;
}

static int read_pos(void* player, float* out) {
    int nu, ok = 0; const char* sname; void* sf; void* sc; char* sp; int shift, poff; void* pf;
    nu = sync_kind(player); g_lastKind = nu;
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
    p->hasPos = read_pos(item, pos); p->src = g_lastKind;
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
    b_s(&b, ",\"me\":{\"id\":"); b_int(&b, meId); b_s(&b, ",\"src\":"); b_int(&b, g_meKind);
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
        b_s(&b, ",\"veh\":"); b_int(&b, p->veh); b_s(&b, ",\"src\":"); b_int(&b, p->src);
        b_ch(&b, '}');
    }
    b_s(&b, "]}");
    g_jsonCur = next;
}

/* ───────────── снимки в HLOG/near_snap.log (вместо http://127.0.0.1:17771/near в браузере) ─────────────
   Команда /nearlog [N] в чате игры: мод запишет N снимков (по одному в секунду, 1..120; без числа — один) рядом с near.log.
   В каждом: ваша позиция и источник (src: 1 пешком, 2 водитель, 3 пассажир), у каждого игрока — ID, ник, d2 (по горизонтали), d (3D),
   src, veh, HP, пинг и координаты. Нужно, чтобы поймать момент, когда расстояния «врут»: набрать /nearlog 20, походить, потом открыть файл. */
static volatile int g_snapLeft;
static long g_snapLast, g_snapBytes;
static char g_snapBuf[24576];
static void snap_write(float* me, int havePos, int meId) {
    char path[320]; Buf b; unsigned i, L; long now = time(NULL);
    if (g_snapLeft <= 0 || now == g_snapLast) return;
    g_snapLast = now; g_snapLeft--;
    L = slen(g_flog);
    if (!g_flog[0] || !ends_with(g_flog, "near.log")) { dlog("снимки: путь к файловому журналу неизвестен — не записано"); g_snapLeft = 0; return; }
    b_init(&b, path, sizeof path);
    for (i = 0; i + 8 < L && b.n + 1 < b.cap; i++) b_ch(&b, g_flog[i]);
    b_s(&b, "near_snap.log");
    if (g_snapBytes > 2000000) { g_snapLeft = 0; dlog("снимки: near_snap.log достиг 2 МБ — остановлено"); return; }
    b_init(&b, g_snapBuf, sizeof g_snapBuf);
    b_s(&b, "== +"); b_int(&b, now - g_flogT0); b_s(&b, "с tick "); b_int(&b, (long)g_ticks);
    b_s(&b, " | я id "); b_int(&b, meId); b_s(&b, " src "); b_int(&b, g_meKind);
    if (havePos) { b_s(&b, " x "); b_fix(&b, me[0], 1); b_s(&b, " y "); b_fix(&b, me[1], 1); b_s(&b, " z "); b_fix(&b, me[2], 1); }
    else b_s(&b, " позиции нет");
    b_s(&b, " | игроков "); b_int(&b, g_np); b_ch(&b, '\n');
    for (i = 0; i < (unsigned)g_np; i++) {
        PInfo* p = &g_pl[i];
        b_s(&b, "  ["); b_int(&b, p->id); b_s(&b, "] "); b_s(&b, p->nick);
        if (p->hasPos && havePos) {
            double dx = p->x - me[0], dy = p->y - me[1], dz = p->z - me[2];
            b_s(&b, "  d2 "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy), 1);
            b_s(&b, " d "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 1);
        } else b_s(&b, "  d ?");
        b_s(&b, "  src "); b_int(&b, p->src); b_s(&b, " veh "); b_int(&b, p->veh);
        b_s(&b, " hp "); b_fix(&b, p->hp, 0); b_s(&b, " ping "); b_fix(&b, p->ping, 0);
        if (p->hasPos) { b_s(&b, "  xyz "); b_fix(&b, p->x, 1); b_ch(&b, ' '); b_fix(&b, p->y, 1); b_ch(&b, ' '); b_fix(&b, p->z, 1); }
        b_ch(&b, '\n');
    }
    if (flog_write(path, b.p, b.n, 0)) g_snapBytes += b.n;
    else { dlog("снимки: не удалось писать в near_snap.log"); g_snapLeft = 0; }
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
        havePos = read_pos(g_local, me); g_meKind = g_lastKind;
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
    snap_write(me, havePos, meId);
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

/* Сколько секунд ждать после появления libil2cpp: по умолчанию NEAR_SETTLE_S, либо число из файла HLOG/near_delay.txt */
static int settle_seconds(void) {
    char p[330], tmp[16]; unsigned L = slen(g_flog); int i, fd, v = -1, k = 0; long r;
    if (L > 9 && L < 300) {
        for (i = 0; i < (int)L - 8; i++) p[i] = g_flog[i];            /* папка со слэшем (без «near.log») */
        p[i] = 0;
        { const char* nm = "near_delay.txt"; for (k = 0; nm[k]; k++) p[i + k] = nm[k]; p[i + k] = 0; }
        fd = open(p, 0);
        if (fd >= 0) {
            r = read(fd, tmp, sizeof tmp - 1); close(fd);
            if (r > 0) {
                tmp[r] = 0; v = 0;
                for (k = 0; k < r && tmp[k] >= '0' && tmp[k] <= '9' && v < 100000; k++) v = v * 10 + (tmp[k] - '0');
                if (k == 0) v = -1;
            }
        }
    }
    if (v < 0) return NEAR_SETTLE_S;
    if (v > 600) v = 600;
    dlog_i("collector: задержка взята из near_delay.txt, секунд ", v);
    return v;
}

static void* collector_main(void* arg) {
    int w, sec, polls = 0, m;
    (void)arg;
    dlog("collector: старт");
    set_stage("ждём загрузку libil2cpp.so"); publish(0, g_stage, 0, 0, 0, 0, -1);
    for (;;) {                                           /* только чтение /proc/self/maps, никаких вызовов dlopen/IL2CPP */
        m = lib_mapped();
        if (m > 0) break;
        if (m < 0 && polls > 60) { dlog("collector: /proc/self/maps недоступен, продолжаю по таймеру"); break; }
        polls++;
        if (polls % 4 == 0) dlog_i("collector: libil2cpp.so ещё не загружена, опросов ", polls);
        usleep(500000);
    }
    sec = settle_seconds();
    dlog_i("collector: libil2cpp.so загружена; функции IL2CPP не трогаю, жду секунд ", sec);
    set_stage("IL2CPP инициализируется (мод ждёт)"); publish(0, g_stage, 0, 0, 0, 0, -1);
    for (w = 0; w < sec; w++) usleep(1000000);
    g_safe = 1;
    dlog("collector: пауза прошла, начинаю опрос IL2CPP");
    set_stage("ждём Assembly-CSharp / Game.Instance"); publish(0, g_stage, 0, 0, 0, 0, -1);
    for (w = 0; !game_ready(); w++) {
        if (w % 20 == 0) dlog_i("collector: игра ещё не готова, опросов ", w);
        usleep(POLL_US);
    }
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

/* ───────────── реконнект (нативная команда /rec) ─────────────
   Повторяет Hassle::Commands::Reconnect оригинального libHassleExtended (/rec): берёт Game.Network (static), читает
   m_Host/m_Port/m_Password/m_NickName, вызывает OnApplicationQuit() (посылает ID_DISCONNECTION_NOTIFICATION, если State == 2)
   и DelayedConnect(host, port, password, nick, секунды) — внутри UniTask.Delay(N) и затем NetworkClient.Connect(...). */
#define REC_MIN_S 0                     /* можно и совсем быстрый /rec (0–2 с), в оригинале было от 5 */
#define REC_MAX_S 600

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

/* ── Немедленный разрыв соединения через RakNet (как делала сборка от 21.10 10:31) ──
   OnApplicationQuit() сам шлёт серверу уведомление об отключении только при State == Connected. Чтобы сервер и игра сразу
   увидели разрыв («Вы были отключены от сервера»), закрываем все соединения RakPeer штатным CloseConnection с уведомлением:
   Game.Network.m_RakNet._nativeClient = RakPeer*; libRakNet.so: RakPeer::GetConnectionList + RakPeer::CloseConnection. */
typedef struct { unsigned long long g; u16 si; } RakGuid;                       /* RakNetGUID: 16 байт */
typedef struct { u8 addr[16]; u16 dport; u16 sidx; } RakSysAddr;                /* SystemAddress без IPv6: 20 байт */
typedef struct { RakGuid guid; RakSysAddr addr; } RakAOG;                       /* AddressOrGUID: 40 байт, передаётся по значению */
typedef int  (*rak_getlist_t)(void* peer, RakSysAddr* out, u16* num);
typedef void (*rak_close_t)(void* peer, RakAOG target, u8 notify, u8 channel, int prio);

static void rak_close_all(void* nc) {
    void *rk, *peer, *h; rak_getlist_t gl = NULL; rak_close_t cl = NULL; const RakGuid* ug = NULL;
    RakSysAddr list[8]; u16 num = 8; int i, n, closed = 0;
    rk = ref_field(nc, "m_RakNet");
    if (!rk) { dlog("реконнект: Game.Network.m_RakNet не найден или пуст"); return; }
    peer = ref_field(rk, "_nativeClient");
    if (!peer) { dlog("реконнект: m_RakNet._nativeClient пуст"); return; }
    h = dlopen("libRakNet.so", RTLD_NOW_ | 4);                                  /* RTLD_NOLOAD: только если уже загружена */
    if (!h) { dlog("реконнект: libRakNet.so / GetConnectionList недоступны"); return; }
    *(void**)&gl = dlsym(h, "_ZNK6RakNet7RakPeer17GetConnectionListEPNS_13SystemAddressEPt");
    *(void**)&cl = dlsym(h, "_ZN6RakNet7RakPeer15CloseConnectionENS_13AddressOrGUIDEbh14PacketPriority");
    ug = (const RakGuid*)dlsym(h, "_ZN6RakNet22UNASSIGNED_RAKNET_GUIDE");
    if (!gl) { dlog("реконнект: libRakNet.so / GetConnectionList недоступны"); return; }
    if (!cl) { dlog("реконнект: RakPeer::CloseConnection не найден в libRakNet.so"); return; }
    { unsigned k; for (k = 0; k < sizeof list; k++) ((u8*)list)[k] = 0; }
    gl(peer, list, &num);
    n = num > 8 ? 8 : (int)num;
    dlog_i("реконнект: активных соединений RakNet: ", n);
    for (i = 0; i < n; i++) {
        RakAOG a; unsigned k;
        for (k = 0; k < sizeof a; k++) ((u8*)&a)[k] = 0;
        if (ug) a.guid = *ug; else { a.guid.g = ~0ULL; a.guid.si = 0xFFFF; }
        a.addr = list[i];
        cl(peer, a, 1, 0, 0);                                                   /* уведомить сервер, канал 0, IMMEDIATE_PRIORITY */
        closed++;
    }
    dlog_i("реконнект: закрыто соединений: ", closed);
}

/* ── Камера → «в воздух», как при заходе в игру, БЕЗ загрузки чего-либо ──
   При входе камера висит в воздухе (мир не рендерится). Повторяем это на /rec в три лёгких шага (всё в главном потоке, исключения
   гасим, при любой неудаче просто выходим):
     1) MainCamera.SetMode(None = 0) — как Game.OnLoadingFinished / RequestSpawn: ResetTarget(), камера отцеплена от персонажа/машины;
     2) get_Position() — где камера сейчас; SetPosition(она + REC_CAM_UP по Y) — поднимаем простую камеру высоко вверх;
     3) SetLookPosition(точка на той же высоте впереди) — смотрим горизонтально, в небо.
   После реконнекта сервер ставит свою камеру (auth-сцена: InterpolateCameraPos), при спавне SetTarget(Ped) вернёт слежение.
   Game.Reset() не вызываем — он зовётся только из PacketReceive и сбрасывает/грузит лишнее. */
#define REC_CAM_UP 3000.0f
static void rec_camera_stock(void) {
    void *fn, *cam = NULL, *k, *m, *exc = NULL, *ret; int mode = 0; float p[3], l[3]; char et[256];
    if (!g_cGame) return;
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) { dlog("реконнект: Game.MainCamera не найдено — камеру не трогаем"); return; }
    A.field_static_get_value(fn, &cam);
    if (!cam) { dlog("реконнект: Game.MainCamera = null — камеру не трогаем"); return; }
    k = A.object_get_class(cam);

    m = find_method(k, "SetMode", 1);
    if (!m) { dlog("реконнект: MainCamera.SetMode не найден"); return; }
    { void* a[1]; a[0] = &mode; A.runtime_invoke(m, cam, a, &exc); }
    if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: MainCamera.SetMode(None) бросил: ", et); return; }

    m = find_method(k, "get_Position", 0);
    if (!m) { dlog("реконнект: MainCamera.get_Position не найден — камера только отцеплена"); return; }
    exc = NULL; ret = A.runtime_invoke(m, cam, NULL, &exc);
    if (exc || !ret) { dlog("реконнект: get_Position не удался — камера только отцеплена"); return; }
    { const float* v = (const float*)((char*)ret + 2 * sizeof(void*));          /* упакованный Vector3: данные сразу за заголовком объекта */
      p[0] = v[0]; p[1] = v[1] + REC_CAM_UP; p[2] = v[2];
      if (!(p[0] == p[0]) || !(p[1] == p[1]) || !(p[2] == p[2])) { dlog("реконнект: позиция камеры не число — камера только отцеплена"); return; } }
    l[0] = p[0]; l[1] = p[1]; l[2] = p[2] + 100.0f;

    m = find_method(k, "SetPosition", 1);
    if (m) { void* a[1]; a[0] = p; exc = NULL; A.runtime_invoke(m, cam, a, &exc);
             if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: MainCamera.SetPosition бросил: ", et); return; } }
    else { dlog("реконнект: MainCamera.SetPosition не найден"); return; }
    m = find_method(k, "SetLookPosition", 1);
    if (m) { void* a[1]; a[0] = l; exc = NULL; A.runtime_invoke(m, cam, a, &exc);
             if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: MainCamera.SetLookPosition бросил: ", et); return; } }
    dlog("реконнект: камера поднята в воздух (SetMode None + SetPosition/SetLookPosition)");
}

/* Общая часть: и для HTTP /rec, и для нативной чат-команды. IL2CPP при необходимости поднимается ЛЕНИВО — набрать /rec можно только
   тогда, когда игра уже идёт, поэтому ждать сборщика (бывшие 60 секунд) не нужно. */
static void do_reconnect(int t, RecRes* r) {
    int state = -1, port = 0, ok = 0;
    void *fn, *nc = NULL, *nk, *mq, *md, *exc = NULL, *host, *pw, *nick;
    unsigned short port16; float tf; char et[256];

    r->ok = 0; r->state = -1; r->port = 0; r->quit = 0; r->why[0] = 0; r->exc[0] = 0;
    if (t < REC_MIN_S) t = REC_MIN_S;                   /* диапазон 0–600 */
    if (t > REC_MAX_S) t = REC_MAX_S;
    r->t = t;
    if (!g_safe) { rec_set(r, "мод Near ещё стартует (ждёт конца загрузки IL2CPP), подождите"); return; }
    if (!il2cpp_light()) { rec_set(r, "IL2CPP ещё не готов (игра грузится, подождите)"); return; }
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

    /* 1) корректный выход с сервера (внутри сам проверяет State == Connected).
       Как в оригинальном libHassleExtended (HassleRec): OnApplicationQuit → DelayedConnect.
       Game.Reset() НЕ вызываем (нагружает, на прошлом клиенте игра вылетала спустя время); кулдауна между /rec нет, как в оригинале;
       прямой разрыв через RakNet (rak_close_all) из /rec убран; камера (rec_camera_stock) возвращена. */
    exc = NULL;
    A.runtime_invoke(mq, nc, NULL, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: OnApplicationQuit бросил: ", et); }
    else r->quit = 1;

    /* 1б) камера — в стоковое (лёгкие сеттеры, ничего не грузим) */
    rec_camera_stock();

    /* 2) подключение через N секунд (внутри UniTask.Delay(N), затем NetworkClient.Connect) */
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
    r->ok = 1; r->state = state; r->port = port;
    dlog("реконнект: DelayedConnect запущен");
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
    if (t < 0) t = 5;                                   /* без аргумента — 5 секунд */
    if (t < REC_MIN_S || t > REC_MAX_S) {               /* допустимо 0–600 */
        chat_say("Используйте: /rec [секунды (0-600]");
        return;
    }
    do_reconnect(t, &r);
    if (r.ok) return;                                   /* успех — в чат ничего не пишем */
    b_s(&b, "Реконнект: "); b_s(&b, r.why);             /* ошибку оставляем, чтобы было видно причину */
    chat_say(msg);
}

/* /nearlog [N] — записать N снимков в HLOG/near_snap.log. Выполняется в главном потоке; сам снимок пишет поток-сборщик (g_pl не трогаем отсюда) */
static void native_nearlog_cmd(void* self, void* args, void* mi) {
    int diag = 0, t; Buf b; char msg[200];
    (void)self; (void)mi;
    t = parse_cmd_args(args, &diag);
    if (t < 1) t = 1;
    if (t > 120) t = 120;
    g_snapLast = 0; g_snapLeft = t;
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Near: пишу снимки в HLOG/near_snap.log ("); b_int(&b, t); b_s(&b, " шт., по одному в секунду)");
    chat_say(msg);
    dlog_i("нативная /nearlog: снимков ", t);
}


/* ───────────── /nocol: проходить сквозь игроков и NPC ─────────────
   /nocol            — переключить (вкл ↔ выкл)
   /nocol on | off   — включить «без столкновений» / вернуть как было (on повторно — захватить тех, кто появился позже)
   /nocol status     — текущее состояние
   Всё выполняется в главном потоке Unity (нативные вызовы Physics из фонового потока небезопасны). Действует только на вашем клиенте.
   Ограничение: новые игроки/NPC, вошедшие в зону после команды, не захватываются сами — повторите /nocol on. После респавна — тоже. */
#define NC_MAXT 640
static void* g_nc_t[NC_MAXT];
static int g_nc_nt;
static void* g_nc_me;
static int g_nc_on, g_nc_lastP, g_nc_lastA, g_nc_lastC, g_nc_logged, g_nc_physTried, g_nc_ctrlDumped;
static void* g_mPhysIgnore;                        /* Physics.IgnoreCollision(Collider, Collider, bool) */
static void* g_nc_cacheCls[6]; static void* g_nc_cacheM[6]; static int g_nc_cacheN;

typedef struct { int players, actors, cols, ign_ok, ign_exc, phys_ok, phys_exc; char err[160]; char exc[240]; } NcRes;
static void nc_err(NcRes* r, const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof r->err - 1; i++) r->err[i] = why[i];
    r->err[i] = 0;
    dlog2("нативная /nocol: отказ: ", r->err);
}

static void* find_image_pre(const char* pre) {
    uptr n = 0, i; void** as;
    if (!g_domain) return NULL;
    as = A.domain_get_assemblies(g_domain, &n);
    for (i = 0; as && i < n; i++) {
        void* img = A.assembly_get_image(as[i]);
        const char* nm = img ? A.image_get_name(img) : NULL;
        if (nm && sprefix(nm, pre)) return img;
    }
    return NULL;
}
/* метод по имени, числу параметров и суффиксу типа первого параметра: у движения две перегрузки IgnoreCollision(Collider|Rigidbody, bool),
   а class_get_method_from_name вернул бы любую из них — подсунуть Collider в параметр Rigidbody нельзя */
static void* find_method_p(void* k, const char* name, int argc, const char* p0suffix) {
    for (; k; k = A.class_get_parent(k)) {
        void* it = NULL; void* m;
        while ((m = A.class_get_methods(k, &it))) {
            const char* mn = A.method_get_name(m); void* t; const char* tn;
            if (!mn || !seq(mn, name)) continue;
            if ((int)A.method_get_param_count(m) != argc) continue;
            if (!p0suffix) return m;
            t = A.method_get_param(m, 0);
            tn = t ? A.type_get_name(t) : NULL;
            if (tn && ends_with(tn, p0suffix)) return m;
        }
    }
    return NULL;
}
static void* nc_ign_method(void* obj) {
    void* k = A.object_get_class(obj); void* m; int i;
    for (i = 0; i < g_nc_cacheN; i++) if (g_nc_cacheCls[i] == k) return g_nc_cacheM[i];
    m = find_method_p(k, "IgnoreCollision", 2, ".Collider");
    if (g_nc_cacheN < 6) { g_nc_cacheCls[g_nc_cacheN] = k; g_nc_cacheM[g_nc_cacheN] = m; g_nc_cacheN++; }
    return m;
}

/* компоненты движения актёра: Actor.m_Controller (Character) → Movement (ECM2 CharacterMovement) и/или SimpleMovement (SimpleCharacterMovement) */
static int nc_movements(void* actor, void** out, int cap) {
    static const char* nm[2] = { "Movement", "SimpleMovement" };
    void* ctrl = ref_field(actor, "m_Controller"); int n = 0, i, j;
    if (!ctrl) {
        void* m = find_method(A.object_get_class(actor), "get_Controller", 0);
        if (m) { void* exc = NULL; ctrl = A.runtime_invoke(m, actor, NULL, &exc); if (exc) ctrl = NULL; }
    }
    if (!ctrl) return 0;
    for (i = 0; i < 2 && n < cap; i++) {
        void* mv = ref_field(ctrl, nm[i]); int dup = 0;
        if (!mv) continue;
        for (j = 0; j < n; j++) if (out[j] == mv) dup = 1;
        if (!dup) out[n++] = mv;
    }
    return n;
}
static void* nc_capsule(void* mv) {
    void* c = ref_field(mv, "_capsuleCollider");       /* ECM2 CharacterMovement */
    if (!c) c = ref_field(mv, "m_Collider");           /* SimpleCharacterMovement */
    return c;
}
/* коллайдер жив и его объект активен (Physics.IgnoreCollision на неактивных ругается ошибкой в логе Unity) */
static int nc_active(void* col) {
    void* m; void* go; void* r; void* exc = NULL;
    m = find_method(A.object_get_class(col), "get_gameObject", 0);
    if (!m) return 1;
    go = A.runtime_invoke(m, col, NULL, &exc);
    if (exc || !go) return 0;
    m = find_method(A.object_get_class(go), "get_activeInHierarchy", 0);
    if (!m) return 1;
    exc = NULL; r = A.runtime_invoke(m, go, NULL, &exc);
    if (exc || !r) return 0;
    return *((u8*)r + HDR) ? 1 : 0;
}
static void nc_collect_cb(void* item, void* ctx) {
    (void)ctx;
    if (!item || item == g_nc_me || g_nc_nt >= NC_MAXT) return;
    g_nc_t[g_nc_nt++] = item;
}
static void nc_call_ign(void* m, void* obj, void* col, u8 ign, NcRes* r) {
    void* args[2]; void* exc = NULL;
    args[0] = col; args[1] = &ign;
    A.runtime_invoke(m, obj, args, &exc);
    if (exc) { r->ign_exc++; if (!r->exc[0]) exc_text(exc, r->exc, sizeof r->exc); }
    else r->ign_ok++;
}
static void nc_call_phys(void* a, void* b, u8 ign, NcRes* r) {
    void* args[3]; void* exc = NULL;
    if (!g_mPhysIgnore || !a || !b) return;
    args[0] = a; args[1] = b; args[2] = &ign;
    A.runtime_invoke(g_mPhysIgnore, NULL, args, &exc);
    if (exc) r->phys_exc++; else r->phys_ok++;
}

static void nc_apply(int ignore, NcRes* r) {
    void *pool, *stream, *apool, *items; void *lmv[2], *lcap[2];
    int nl, i, t, k, n0; u8 ign = ignore ? 1 : 0;
    r->players = r->actors = r->cols = r->ign_ok = r->ign_exc = r->phys_ok = r->phys_exc = 0;
    r->err[0] = 0; r->exc[0] = 0;
    if (!g_safe) { nc_err(r, "мод Near ещё стартует, подождите"); return; }
    if (!il2cpp_light()) { nc_err(r, "IL2CPP ещё не готов"); return; }
    ensure_attached();
    if (!A.class_get_methods || !A.method_get_name || !A.method_get_param_count || !A.method_get_param) {
        nc_err(r, "в libil2cpp нет il2cpp_class_get_methods / method_get_param"); return;
    }
    if (!g_poolSrcField) { nc_err(r, "пул игроков ещё не найден, подождите пару секунд"); return; }
    pool = current_pool();
    if (!pool) { nc_err(r, "вы не в игре (пул игроков не создан)"); return; }
    g_nc_me = ref_field(pool, "LocalPlayer");
    if (!g_nc_me) { nc_err(r, "ваш персонаж ещё не создан"); return; }
    nl = nc_movements(g_nc_me, lmv, 2);
    if (nl <= 0) {
        nc_err(r, "у вашего персонажа не найден компонент движения (подробности в near.log)");
        if (!g_nc_ctrlDumped) {
            void* ctrl = ref_field(g_nc_me, "m_Controller");
            g_nc_ctrlDumped = 1;
            dump_class(ctrl ? A.object_get_class(ctrl) : A.object_get_class(g_nc_me), ctrl ? "контроллер игрока (Character)" : "LocalPlayer (нет m_Controller)");
        }
        return;
    }
    for (i = 0; i < nl; i++) lcap[i] = nc_capsule(lmv[i]);
    if (!g_nc_physTried) {
        void* img = find_image_pre("UnityEngine.PhysicsModule");
        void* kp = img ? A.class_from_name(img, "UnityEngine", "Physics") : NULL;
        g_nc_physTried = 1;
        if (kp) g_mPhysIgnore = A.class_get_method_from_name(kp, "IgnoreCollision", 3);
        dlog2("нативная /nocol: Physics.IgnoreCollision ", g_mPhysIgnore ? "найден" : "НЕ найден (работаю только через IgnoreCollision движения)");
    }
    if (!g_nc_logged) {
        g_nc_logged = 1;
        for (i = 0; i < nl; i++) {
            dlog2("нативная /nocol: движение игрока: ", A.class_get_name(A.object_get_class(lmv[i])));
            dlog2("нативная /nocol:    его капсула: ", lcap[i] ? A.class_get_name(A.object_get_class(lcap[i])) : "НЕТ");
            dlog2("нативная /nocol:    IgnoreCollision(Collider,bool): ", nc_ign_method(lmv[i]) ? "найден" : "НЕ найден");
        }
    }

    g_nc_nt = 0;
    stream = ref_field(pool, "PlayersInStream");
    if (stream) walk_collection_x(stream, nc_collect_cb, NULL, 1);
    r->players = g_nc_nt; n0 = g_nc_nt;
    apool = current_actor_pool();
    if (apool) { items = ref_field(apool, "m_Items"); if (items) walk_collection_x(items, nc_collect_cb, NULL, 1); }
    r->actors = g_nc_nt - n0;

    for (t = 0; t < g_nc_nt; t++) {
        void *tm[2], *cols[2]; int nt = nc_movements(g_nc_t[t], tm, 2), nc = 0;
        for (k = 0; k < nt; k++) {
            void* c = nc_capsule(tm[k]); int dup = 0, j;
            if (!c) continue;
            for (j = 0; j < nc; j++) if (cols[j] == c) dup = 1;
            if (!dup) cols[nc++] = c;
        }
        for (k = 0; k < nc; k++) {
            if (!nc_active(cols[k])) continue;
            r->cols++;
            for (i = 0; i < nl; i++) {
                void* m;
                if (cols[k] == lcap[i]) continue;
                m = nc_ign_method(lmv[i]);                         /* ваше движение игнорирует чужую капсулу */
                if (m) nc_call_ign(m, lmv[i], cols[k], ign, r);
                nc_call_phys(lcap[i], cols[k], ign, r);            /* и на уровне физики Unity */
            }
        }
        for (k = 0; k < nt; k++) {                                 /* обратно: движение цели игнорирует вашу капсулу (если она сама толкает вас) */
            void* tc = nc_capsule(tm[k]); void* m;
            if (!tc || !nc_active(tc)) continue;                   /* неактивные пропускаем — Unity на них пишет ошибку */
            m = nc_ign_method(tm[k]);
            if (!m) continue;
            for (i = 0; i < nl; i++) if (lcap[i] && lcap[i] != tc) nc_call_ign(m, tm[k], lcap[i], ign, r);
        }
    }
    {
        char t2[300]; Buf x; b_init(&x, t2, sizeof t2);
        b_s(&x, "нативная /nocol: "); b_s(&x, ign ? "ВКЛ" : "ВЫКЛ"); b_s(&x, ": игроков "); b_int(&x, r->players);
        b_s(&x, ", NPC "); b_int(&x, r->actors); b_s(&x, ", капсул "); b_int(&x, r->cols);
        b_s(&x, ", IgnoreCollision ок/ошибок "); b_int(&x, r->ign_ok); b_ch(&x, '/'); b_int(&x, r->ign_exc);
        b_s(&x, ", Physics ок/ошибок "); b_int(&x, r->phys_ok); b_ch(&x, '/'); b_int(&x, r->phys_exc);
        dlog(t2);
        if (r->exc[0]) dlog2("нативная /nocol: первое исключение: ", r->exc);
    }
}

/* слова из аргументов команды (по пробелам, без учёта регистра): 1 — on/1, 2 — off/0, 8 — status/diag */
static int nc_parse(void* arr) {
    long len; int i, w = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[96]; int n, p = 0; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 60 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        while (tmp[p]) {
            char tok[24]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 23) { char c = tmp[p]; tok[q++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } p++; }
            tok[q] = 0;
            if (seq(tok, "on") || seq(tok, "1")) w |= 1;
            else if (seq(tok, "off") || seq(tok, "0")) w |= 2;
            else if (seq(tok, "status") || seq(tok, "diag")) w |= 8;
        }
    }
    return w;
}

static void native_nocol_cmd(void* self, void* args, void* mi) {
    int w, want; NcRes r; Buf b; char msg[420];
    (void)self; (void)mi;
    w = nc_parse(args);
    dlog_i("нативная /nocol: вызвана, флаги ", w);
    b_init(&b, msg, sizeof msg);
    if (w & 8) {
        b_s(&b, "Коллизия с игроками и NPC: "); b_s(&b, g_nc_on ? "ВЫКЛЮЧЕНА" : "включена (как в игре)");
        if (g_nc_on) { b_s(&b, " — игроков "); b_int(&b, g_nc_lastP); b_s(&b, ", NPC "); b_int(&b, g_nc_lastA); b_s(&b, ", капсул "); b_int(&b, g_nc_lastC); }
        chat_say(msg);
        return;
    }
    want = (w & 2) ? 0 : (w & 1) ? 1 : !g_nc_on;
    nc_apply(want, &r);
    if (r.err[0]) { b_s(&b, "Коллизия: не вышло — "); b_s(&b, r.err); chat_say(msg); return; }
    if (r.cols > 0 && r.ign_ok == 0 && r.phys_ok == 0) {
        b_s(&b, "Коллизия: вызов не прошёл ("); b_int(&b, r.ign_exc); b_s(&b, " ошибок), подробности в HLOG/near.log");
        chat_say(msg);
        return;
    }
    g_nc_on = want; g_nc_lastP = r.players; g_nc_lastA = r.actors; g_nc_lastC = r.cols;
    if (!want) { b_s(&b, "Коллизия с игроками и NPC включена обратно"); chat_say(msg); return; }
    if (r.cols == 0) {
        b_s(&b, "Коллизия: рядом нет ни игроков, ни NPC с капсулой. Когда появятся — снова /nocol on");
        chat_say(msg);
        return;
    }
    b_s(&b, "Коллизия с игроками и NPC ВЫКЛЮЧЕНА: игроков "); b_int(&b, r.players); b_s(&b, ", NPC "); b_int(&b, r.actors);
    b_s(&b, ". Тех, кто появится позже, добавит повторный /nocol on. Вернуть: /nocol off");
    chat_say(msg);
}

static void cmd_fail(const char* why) { g_cmdDone = -1; dlog2("нативная /rec: не получилось. Причина: ", why); }
static int cmd_put(void* dict, void* setitem, const char* key, void* del) {
    void* args[2]; void* exc = NULL; char et[256];
    args[0] = A.string_new(key); args[1] = del;
    A.runtime_invoke(setitem, dict, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("нативная /rec: set_Item бросил: ", et); return 0; }
    return 1;
}
/* копия делегата-образца с подменой method_ptr (и invoke_impl, если он совпадал с method_ptr) на нашу функцию */
static void* clone_cmd_delegate(void* kls, int size, int off_mp, int off_ii, uptr fn) {
    void* nd = A.object_new(kls); int i; uptr mp, ii;
    if (!nd) return NULL;
    for (i = 2 * (int)sizeof(void*); i < size; i++) ((char*)nd)[i] = ((char*)g_tplDelegate)[i];
    mp = *(uptr*)((char*)g_tplDelegate + off_mp);
    *(uptr*)((char*)nd + off_mp) = fn;
    if (off_ii) {
        ii = *(uptr*)((char*)g_tplDelegate + off_ii);
        if (ii == mp) *(uptr*)((char*)nd + off_ii) = fn;
    }
    return nd;
}
static void try_register_cmd(void) {
    void *cw, *fd, *dict = NULL, *kls, *nd, *mset, *f_mp, *f_ii; int size, off_mp, off_ii;
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

    nd = clone_cmd_delegate(kls, size, off_mp, f_ii ? off_ii : 0, (uptr)native_rec_cmd);
    if (!nd) { cmd_fail("object_new вернул null"); return; }
    dlog_i("нативная /rec: записей в реестре ", g_dictCount);
    dlog_i("нативная /rec: ключи начинаются с «/» ", g_keyHasSlash);
    if (cmd_put(dict, mset, "/rec", nd) & cmd_put(dict, mset, "rec", nd)) {
        g_cmdDone = 1;
        dlog("нативная /rec: зарегистрирована («/rec» и «rec»)");
        nd = clone_cmd_delegate(kls, size, off_mp, f_ii ? off_ii : 0, (uptr)native_nearlog_cmd);
        if (nd && (cmd_put(dict, mset, "/nearlog", nd) & cmd_put(dict, mset, "nearlog", nd))) dlog("нативная /nearlog: зарегистрирована («/nearlog» и «nearlog»)");
        else dlog("нативная /nearlog: НЕ зарегистрирована");
        nd = clone_cmd_delegate(kls, size, off_mp, f_ii ? off_ii : 0, (uptr)native_nocol_cmd);
        if (nd && (cmd_put(dict, mset, "/nocol", nd) & cmd_put(dict, mset, "nocol", nd))) dlog("нативная /nocol: зарегистрирована («/nocol» и «nocol»)");
        else dlog("нативная /nocol: НЕ зарегистрирована");
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
                { static int hl; if (hl < 15) { char q[48]; int k; hl++; for (k = 0; k < 47 && rq[k] && rq[k] != '\r' && rq[k] != '\n'; k++) q[k] = rq[k]; q[k] = 0; dlog2("HTTP: запрос ", q); } }
                if (sprefix(rq, "OPTIONS ")) respond(c, "204 No Content", "text/plain", "", 0);
                else if (sprefix(rq, "GET /near")) { const char* j = g_json[g_jsonCur]; respond(c, "200 OK", "application/json", j, slen(j)); }
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
