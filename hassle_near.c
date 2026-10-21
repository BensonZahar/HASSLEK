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
 * Нативная команда /rec (без JS и без портов)
 *   • Игра сама держит реестр чат-команд: WebViewSystem.RegisterChatCommandEvent(string, ChatCommandEvent) — так же регистрируются
 *     /q и /quit. Мы регистрируем «/rec» своим делегатом (native_rec_cmd), поэтому команда срабатывает прямо в чате игры, на
 *     главном потоке Unity, и ничего не отправляет на сервер. rec.js и порт 17771 для /rec больше не нужны.
 *     HTTP-эндпоинт /rec оставлен как запасной путь (старый rec.js продолжит работать); порты по-прежнему нужны HUD для /near.
 *   • Паузы «60 секунд после запуска» больше нет: сборщик ждёт реальной готовности (libil2cpp → домен → 3 с на стабилизацию →
 *     Assembly-CSharp), а команда регистрируется, как только WebViewSystem.IsLoaded == true.
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

#define RTLD_NOW_ 2
#define NEAR_PORT_FIRST 17771
#define NEAR_PORT_COUNT 10
#define TICK_US 300000
#define SETTLE_S 3            /* сколько секунд ждём после появления домена IL2CPP, прежде чем перебирать сборки (иначе гонка с инициализацией) */

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

static void dlog(const char* s) {
    if (!g_diag_ready) { b_init(&g_diag, g_diag_mem, sizeof g_diag_mem); g_diag_ready = 1; }
    if (g_diag.n + 200 > g_diag.cap) {                  /* кольцо: когда заполнилось — начинаем сначала */
        g_diag.n = 0; g_diag.p[0] = 0;
        b_s(&g_diag, "[журнал переполнен, начат заново]\n");
    }
    b_s(&g_diag, s); b_ch(&g_diag, '\n');
    if (g_alog) g_alog(4, "HassleNear", s);
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
    void* (*object_new)(void*);
    void* (*string_new)(const char*);
    void* (*class_get_methods)(void*, void**);
    const char* (*method_get_name)(void*);
    unsigned (*method_get_param_count)(void*);
    void* (*method_get_param)(void*, unsigned);
    void (*gc_wbarrier_set_field)(void*, void**, void*);
} A;

static void* g_il2cpp;
static void* g_domain;
static int g_attached;
static long g_domainSince;

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
    LOAD(object_new); LOAD(string_new); LOAD(class_get_methods); LOAD(method_get_name);
    LOAD(method_get_param_count); LOAD(method_get_param); LOAD(gc_wbarrier_set_field);
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

static void try_register_rec(void);

static void collect_tick(void) {
    void* pool; void* stream; float me[3] = {0, 0, 0}; int havePos = 0, meId = -1;
    g_ticks++;
    if (!il2cpp_open()) { set_stage("ждём libil2cpp"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_domain) g_domain = A.domain_get();
    if (!g_domain) { set_stage("ждём домен IL2CPP"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_domainSince) { g_domainSince = time(NULL); dlog("домен IL2CPP появился, даём ему стабилизироваться"); }
    if (time(NULL) - g_domainSince < SETTLE_S) { set_stage("ждём инициализацию IL2CPP"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_img) g_img = find_image();
    if (!g_img) { set_stage("ждём Assembly-CSharp"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_cPlayerPool) {
        g_cPlayerPool = A.class_from_name(g_img, "Hassle.Global.Pooling", "PlayerPool");
        g_cPools = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
        if (!g_cPlayerPool) { set_stage("класс PlayerPool не найден"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    }
    if (!g_attached) { A.thread_attach(g_domain); g_attached = 1; dlog("поток присоединён к IL2CPP"); }
    try_register_rec();
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

static void* collector_main(void* arg) {
    (void)arg;
    init_log();
    dlog("collector: старт (без фиксированной паузы: ждём готовность IL2CPP)");
    for (;;) {
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
static int g_port;
static __thread int tl_attached;
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
/* Результат реконнекта (общий для HTTP /rec и нативной чат-команды /rec) */
typedef struct { int ok; int t, state, port, quit; char why[200]; char exc[256]; } RecRes;

static void rec_why(RecRes* r, const char* why) {
    unsigned i;
    r->ok = 0;
    for (i = 0; why[i] && i < sizeof r->why - 1; i++) r->why[i] = why[i];
    r->why[i] = 0;
    dlog2("реконнект: отказ: ", r->why);
}
/* IL2CPP можно открыть и «лениво» — команда вызывается из чата, значит игра уже работает */
static int ensure_ready(void) {
    if (!il2cpp_open()) return 0;
    if (!g_domain) g_domain = A.domain_get();
    if (!g_domain) return 0;
    if (!g_img) g_img = find_image();
    return g_img != NULL;
}
static void rec_core(int t, RecRes* r) {
    int state = -1, port = 0, quit_called = 0, ok = 0;
    void *cg, *fn, *nc = NULL, *nk, *mq, *md, *exc = NULL;
    void *host, *pw, *nick;
    unsigned short port16; float tf;
    char et[256]; long now;

    r->ok = 0; r->why[0] = 0; r->exc[0] = 0; r->t = t; r->state = -1; r->port = 0; r->quit = 0;
    if (t < 0) t = 0;
    if (t > 600) t = 600;
    r->t = t;
    if (!ensure_ready()) { rec_why(r, "мод ещё не готов (игра грузится, подождите)"); return; }
    now = time(NULL);
    if (now < g_recUntil) { rec_why(r, "реконнект уже запущен, подождите"); return; }
    if (!tl_attached) { A.thread_attach(g_domain); tl_attached = 1; }

    cg = A.class_from_name(g_img, "Hassle", "Game");
    if (!cg) { rec_why(r, "класс Hassle.Game не найден"); return; }
    fn = A.class_get_field_from_name(cg, "Network");
    if (!fn) { rec_why(r, "поле Game.Network не найдено"); return; }
    A.field_static_get_value(fn, &nc);
    if (!nc) { rec_why(r, "Game.Network = null (вы не на сервере?)"); return; }
    nk = A.object_get_class(nc);

    state = (int)num_field(nc, "State", &ok);
    host = ref_field(nc, "m_Host");
    pw = ref_field(nc, "m_Password");
    nick = ref_field(nc, "m_NickName");
    port = (int)num_field(nc, "m_Port", &ok);
    if (!host || !nick) { rec_why(r, "нет данных подключения (m_Host/m_NickName пусты)"); return; }
    mq = find_method(nk, "OnApplicationQuit", 0);
    md = find_method(nk, "DelayedConnect", 5);
    if (!mq || !md) { rec_why(r, !md ? "метод DelayedConnect не найден" : "метод OnApplicationQuit не найден"); return; }
    r->state = state; r->port = port;

    dlog_i("реконнект: старт, секунд ", t);
    dlog_i("реконнект: State ", state);
    dlog_i("реконнект: порт ", port);

    /* 1) корректный выход с сервера (внутри сам проверяет State == Connected) */
    exc = NULL;
    A.runtime_invoke(mq, nc, NULL, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: OnApplicationQuit бросил: ", et); }
    else quit_called = 1;
    r->quit = quit_called;

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
        exc_text(exc, r->exc, sizeof r->exc);
        dlog2("реконнект: DelayedConnect бросил: ", r->exc);
        { const char* w = "DelayedConnect: исключение"; unsigned i; for (i = 0; w[i]; i++) r->why[i] = w[i]; r->why[i] = 0; }
        r->ok = 0;
        return;
    }
    g_recUntil = now + t + 3;
    r->ok = 1;
    dlog("реконнект: DelayedConnect запущен");
}

/* HTTP: GET /rec?t=N — запасной путь (старый rec.js) */
static void handle_rec(int c, const char* rq) {
    RecRes r; Buf b;
    rec_core(query_int(rq, "t", 0), &r);
    b_init(&b, g_rec_out, sizeof g_rec_out);
    if (!r.ok) {
        b_s(&b, "{\"ok\":0,\"why\":"); b_jstr(&b, r.why);
        if (r.exc[0]) { b_s(&b, ",\"exc\":"); b_jstr(&b, r.exc); }
        b_ch(&b, '}');
    } else {
        b_s(&b, "{\"ok\":1,\"t\":"); b_int(&b, r.t);
        b_s(&b, ",\"state\":"); b_int(&b, r.state);
        b_s(&b, ",\"port\":"); b_int(&b, r.port);
        b_s(&b, ",\"quit\":"); b_int(&b, r.quit);
        b_ch(&b, '}');
    }
    respond(c, "200 OK", "application/json", g_rec_out, b.n);
}

/* ───────────── нативная чат-команда /rec ─────────────
   Делегат WebViewSystem.ChatCommandEvent (void Invoke(string[] args)) собираем вручную: берём настоящий объект делегата,
   настоящий MethodInfo его Invoke (нужен для проверок в сгенерированном коде: «не static, 1 параметр») и подставляем
   в method_ptr нашу функцию. IL2CPP вызывает её как (target, args, MethodInfo*) — на главном потоке Unity, из SendChatInput. */
static int g_cmdRegistered, g_cmdTries;
static void* g_mAddChat;               /* WebViewSystem.AddChatMessage(string, string) — для ответов в чат */

static void chat_msg(const char* utf8) {
    void* args[2]; void* exc = NULL;
    if (!g_mAddChat || !A.string_new) return;
    args[0] = A.string_new(utf8); args[1] = A.string_new("#FFFFFF");
    A.runtime_invoke(g_mAddChat, NULL, args, &exc);
}
/* args: string[] (может содержать и саму команду). Секунды: первое число 1..600 (по умолчанию 5), слово diag — диагностика */
static void parse_rec_args(void* arr, int* t, int* diag) {
    long len, i; char* data;
    *t = 5; *diag = 0;
    if (!arr) return;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char a[16]; int n, k, digits = 1; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (!ch || n <= 0 || n > 15) continue;
        for (k = 0; k < n; k++) {
            u16 u = ch[k];
            if (u >= 'A' && u <= 'Z') u = (u16)(u + 32);
            a[k] = u < 128 ? (char)u : '?';
            if (a[k] < '0' || a[k] > '9') digits = 0;
        }
        a[n] = 0;
        if (seq(a, "diag")) *diag = 1;
        else if (digits && n <= 4) {
            int v = 0; for (k = 0; k < n; k++) v = v * 10 + (a[k] - '0');
            if (v < 1) v = 1;
            if (v > 600) v = 600;
            *t = v;
        }
    }
}
static void native_rec_cmd(void* self, void* args, void* mi) {
    int t, diag; RecRes r; char m[300]; Buf b;
    (void)self; (void)mi;
    parse_rec_args(args, &t, &diag);
    b_init(&b, m, sizeof m);
    if (diag) {
        b_s(&b, "[REC] мод Near: "); b_s(&b, g_stage); b_s(&b, ", игроков рядом: "); b_int(&b, g_np);
        b_s(&b, ", HTTP-порт: "); b_int(&b, g_port);
        dlog2("/rec diag: ", m); chat_msg(m);
        return;
    }
    rec_core(t, &r);
    if (r.ok) { b_s(&b, "[REC] отключено, подключение через "); b_int(&b, t); b_s(&b, " сек"); }
    else { b_s(&b, "[REC] "); b_s(&b, r.why); if (r.exc[0]) { b_s(&b, ": "); b_s(&b, r.exc); } }
    chat_msg(m);
}

static void try_register_rec(void) {
    void *cw, *f, *it = NULL, *m, *mReg = NULL, *ty, *ccls, *mInv, *d, *exc = NULL, *tgt;
    unsigned char loaded = 0; int off;
    if (g_cmdRegistered || g_cmdTries >= 5) return;
    cw = A.class_from_name(g_img, "", "WebViewSystem");
    if (!cw) return;
    f = A.class_get_field_from_name(cw, "IsLoaded");
    if (!f) { g_cmdTries = 5; dlog("/rec: поле WebViewSystem.IsLoaded не найдено — команду не регистрирую"); return; }
    A.field_static_get_value(f, &loaded);
    if (!loaded) return;                                   /* интерфейс ещё грузится — Game.Awake уже точно отработал после этого */
    g_cmdTries++;
    while ((m = A.class_get_methods(cw, &it))) {           /* два перегруза: (string[], ...) и (string, ...) — нужен второй */
        const char* nm = A.method_get_name(m);
        if (nm && seq(nm, "RegisterChatCommandEvent") && A.method_get_param_count(m) == 2) {
            const char* pn; ty = A.method_get_param(m, 0);
            pn = ty ? A.type_get_name(ty) : NULL;
            if (pn && seq(pn, "System.String")) { mReg = m; break; }
        }
    }
    if (!mReg) { dlog("/rec: WebViewSystem.RegisterChatCommandEvent(string, ...) не найден"); return; }
    ty = A.method_get_param(mReg, 1);
    ccls = ty ? A.class_from_type(ty) : NULL;
    mInv = ccls ? find_method(ccls, "Invoke", 1) : NULL;
    if (!ccls || !mInv) { dlog("/rec: класс делегата ChatCommandEvent или его Invoke не найден"); return; }
    {
        void* fp = find_field(ccls, "method_ptr"); void* ft = find_field(ccls, "m_target"); void* fm = find_field(ccls, "method");
        if (!fp || !ft || !fm) { dlog("/rec: у System.Delegate нет полей method_ptr/m_target/method"); g_cmdTries = 5; return; }
        d = A.object_new(ccls);
        if (!d) { dlog("/rec: не удалось создать делегат"); return; }
        tgt = A.string_new("rec");                          /* любой ненулевой объект: на него ссылается делегат, GC его не соберёт */
        *(void**)((char*)d + A.field_get_offset(fp)) = (void*)native_rec_cmd;
        *(void**)((char*)d + A.field_get_offset(fm)) = mInv;
        off = A.field_get_offset(ft);
        A.gc_wbarrier_set_field(d, (void**)((char*)d + off), tgt);
    }
    {
        void* args[2];
        args[0] = A.string_new("/rec"); args[1] = d;
        A.runtime_invoke(mReg, NULL, args, &exc);
    }
    if (exc) { char et[256]; exc_text(exc, et, sizeof et); dlog2("/rec: RegisterChatCommandEvent бросил: ", et); return; }
    g_mAddChat = NULL;
    it = NULL;
    while ((m = A.class_get_methods(cw, &it))) {
        const char* nm = A.method_get_name(m);
        if (nm && seq(nm, "AddChatMessage") && A.method_get_param_count(m) == 2) { g_mAddChat = m; break; }
    }
    g_cmdRegistered = 1;
    dlog("/rec: нативная чат-команда зарегистрирована");
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
    dlog("start_threads: запускаю сервер и сборщик");
    pthread_create(&t1, NULL, server_main, NULL);
    pthread_create(&t2, NULL, collector_main, NULL);
}

#ifndef NEAR_TEST
typedef int (*jni_onload_t)(void*, void*);
typedef void (*jni_onunload_t)(void*, void*);
static void* g_orig;

/* Оригинал лежит рядом: libHassleExtend2.so (старый клиент) или libHassleMain2.so (чистый APK, подмена libmain.so) */
EXPORT int JNI_OnLoad(void* vm, void* reserved) {
    int ver = 0x00010006;                                  /* JNI_VERSION_1_6 */
    init_log();
    dlog("JNI_OnLoad: вход, грузим оригинал (libHassleExtend2.so / libHassleMain2.so)");
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
    } else dlog("JNI_OnLoad: оригинал (libHassleExtend2.so / libHassleMain2.so) НЕ загрузился");
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
