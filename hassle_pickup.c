/*
 * hassle_pickup.c — необязательный модуль «/pk»: заново сработать на метке/пикапе, у которой вы уже стоите
 * (раздевалка, магазин, лифт и т.п. — всё, что открывается «при подходе»). Только ваш клиент, сервер ничего не знает.
 *
 * Зачем. Вход в пикап определяет клиент: Unity зовёт Pickup.OnTriggerEnter(Collider), и клиент шлёт серверу RPC PickedUpPickup
 * с номером пикапа. Unity вызывает OnTriggerEnter один раз при входе в зону, поэтому после закрытия окна приходится отойти и
 * подойти снова. Команда /pk делает то же самое вручную: берёт ближайший к вам пикап и зовёт его OnTriggerEnter с вашей капсулой —
 * клиент отправляет тот же RPC, что и при обычном входе.
 *
 * Команда в чате (нативная, как /rec и /nocol):
 *   /pk        ближайший пикап в пределах 5 м
 *   /pk 10     то же, но в пределах 10 м (1…20)
 *
 * hassle_near.c НЕ МЕНЯЕТСЯ. Этот файл включает hassle_nocol.c (если он лежит рядом) или hassle_near.c и добавляет /pk сверху.
 * build_near.py собирает самый «верхний» из файлов: hassle_pickup.c → hassle_nocol.c → hassle_near.c.
 * КАК УБРАТЬ: удалите hassle_pickup.c и пересоберите .so — остальное работает как раньше (/nocol, если файл есть, остаётся).
 */
#if defined(__has_include)
#  if __has_include("hassle_nocol.c")
#    include "hassle_nocol.c"
#  else
#    include "hassle_near.c"
#  endif
#else
#  include "hassle_near.c"
#endif

#ifndef NEAR_REG_LOCK
#define NEAR_REG_LOCK
static volatile int g_near_regLock;
static void near_reg_lock(void) { while (__sync_lock_test_and_set(&g_near_regLock, 1)) usleep(1000); }
static void near_reg_unlock(void) { __sync_lock_release(&g_near_regLock); }
#endif

/* ───────────── безопасное чтение памяти (свои копии, чтобы файл не зависел от hassle_nocol.c) ───────────── */
extern int pipe(int*);
extern int fcntl(int, int, ...);
static int g_pk_pipe[2] = { -1, -1 }; static volatile int g_pk_pipeLock;
static int pk_readable(const void* p, unsigned n) {
    char buf[64]; long w; int ok;
    if (!p || (uptr)p < 0x10000 || n == 0 || n > sizeof buf) return 0;
    while (__sync_lock_test_and_set(&g_pk_pipeLock, 1)) usleep(50);
    if (g_pk_pipe[0] == -1) {
        if (pipe(g_pk_pipe)) g_pk_pipe[0] = -2;
        else { fcntl(g_pk_pipe[0], 4, 0x800); fcntl(g_pk_pipe[1], 4, 0x800); }
    }
    if (g_pk_pipe[0] < 0) { __sync_lock_release(&g_pk_pipeLock); return 1; }
    w = write(g_pk_pipe[1], p, n);
    ok = (w == (long)n);
    if (w > 0) while (read(g_pk_pipe[0], buf, sizeof buf) > 0) {}
    __sync_lock_release(&g_pk_pipeLock);
    return ok;
}
static int pk_obj_ok(const void* o) { return o && pk_readable(o, 16) && pk_readable(*(void* const*)o, 16); }
static int pk_range_ok(const void* p, long bytes) {
    long o;
    if (!p || bytes <= 0 || bytes > (1L << 22)) return 0;
    for (o = 0; o < bytes; o += 4096) if (!pk_readable((const char*)p + o, 1)) return 0;
    return pk_readable((const char*)p + bytes - 1, 1);
}
/* ссылочное поле с проверкой типа и памяти */
static void* pk_ref(void* obj, const char* name) {
    void *k, *f; int tk, off;
    if (!pk_obj_ok(obj)) return NULL;
    k = A.object_get_class(obj);
    f = find_field(k, name);
    if (!f) return NULL;
    tk = A.type_get_type(A.field_get_type(f));
    if (!(tk == 0x12 || tk == 0x15 || tk == 0x1c || tk == 0x0e || tk == 0x1d || tk == 0x14)) return NULL;
    off = A.field_get_offset(f);
    if (off < 0 || off > 0x2000 || !pk_readable((char*)obj + off, sizeof(void*))) return NULL;
    return *(void**)((char*)obj + off);
}
static void* pk_find_image(const char* pre) {
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

/* ───────────── данные игры ───────────── */
static void* g_pk_mTrig;                                  /* Pickup.OnTriggerEnter(Collider) */
static void* g_pk_trigCls;
#define PK_MAX 256
static void* g_pk_list[PK_MAX]; static int g_pk_n;

static void pk_collect_cb(void* item, void* ctx) { (void)ctx; if (item && g_pk_n < PK_MAX) g_pk_list[g_pk_n++] = item; }

/* Pools.Pickups — static-поле; внутри BasePool<Pickup>.m_Items — Dictionary<int, Pickup> */
static int pk_collect(void) {
    void *f, *pool = NULL, *dict, *arr; long len; int esz;
    g_pk_n = 0;
    if (!g_cPools) return 0;
    f = A.class_get_field_from_name(g_cPools, "Pickups");
    if (!f || !(A.field_get_flags(f) & 0x10)) return 0;
    A.field_static_get_value(f, &pool);
    dict = pk_ref(pool, "m_Items");
    if (!dict) return 0;
    arr = pk_ref(dict, "_entries");
    if (!pk_obj_ok(arr) || !pk_readable(arr, 32)) return 0;
    esz = A.class_array_element_size(A.object_get_class(arr));
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (esz <= 0 || len < 0 || len > 100000) return 0;
    if (len > 0 && !pk_range_ok(arr, 4 * (long)sizeof(void*) + len * esz)) return 0;
    walk_struct_array(arr, int_field(dict, "_count", NULL, NULL, -1), "value", pk_collect_cb, NULL);
    return g_pk_n;
}
/* ваша капсула: LocalPlayer.m_Controller → Movement → _capsuleCollider / m_Collider */
static void* pk_local_capsule(void) {
    void *pool, *me, *ctrl, *mv, *c;
    pool = current_pool();
    if (!pool) return NULL;
    me = pk_ref(pool, "LocalPlayer");
    ctrl = pk_ref(me, "m_Controller");
    mv = pk_ref(ctrl, "Movement");
    if (!mv) return NULL;
    c = pk_ref(mv, "_capsuleCollider");
    if (!c) c = pk_ref(mv, "m_Collider");
    return c;
}
/* позиция компонента: get_transform → Transform.get_position (Vector3 приходит упакованным: три float сразу за заголовком) */
static int pk_pos(void* comp, float* out) {
    void *m, *tr, *r, *exc = NULL;
    if (!pk_obj_ok(comp)) return 0;
    m = find_method(A.object_get_class(comp), "get_transform", 0);
    if (!m) return 0;
    tr = A.runtime_invoke(m, comp, NULL, &exc);
    if (exc || !pk_obj_ok(tr)) return 0;
    m = find_method(A.object_get_class(tr), "get_position", 0);
    if (!m) return 0;
    exc = NULL; r = A.runtime_invoke(m, tr, NULL, &exc);
    if (exc || !pk_obj_ok(r)) return 0;
    memcpy(out, (char*)r + HDR, 12);
    return 1;
}

/* ───────────── команда ───────────── */
static int pk_radius(void* arr) {                           /* число метров из аргументов, 0 — не задано */
    long len; int i, r = 0; char* data;
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
            int v = 0, digits = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] >= '0' && tmp[p] <= '9') { if (digits < 3) v = v * 10 + (tmp[p] - '0'); digits++; p++; }
            if (digits && (tmp[p] == 0 || tmp[p] == ' ')) { if (v >= 1 && v <= 20) r = v; }
            while (tmp[p] && tmp[p] != ' ') p++;
        }
    }
    return r;
}
static void pk_say(const char* a, long num, const char* b) {
    char msg[300]; Buf x; b_init(&x, msg, sizeof msg);
    b_s(&x, a); if (num >= 0) b_int(&x, num); b_s(&x, b);
    chat_say(msg);
}

static void native_pk_cmd(void* self, void* args, void* mi) {
    float me[3], p[3], bestd2 = 0, allD2 = 0; int radius, i, best = -1, inRange = 0, allIdx = -1, failed = 0; void *lcap, *bestObj = NULL, *exc = NULL, *a[1];
    (void)self; (void)mi;
    dlog("нативная /pk: вызвана");
    if (!g_safe) { chat_say("Метка: мод Near ещё стартует, подождите"); return; }
    if (!il2cpp_light()) { chat_say("Метка: IL2CPP ещё не готов"); return; }
    ensure_attached();
    if (!g_poolSrcField || !current_pool()) { chat_say("Метка: вы не в игре (пул игроков не создан)"); return; }
    radius = pk_radius(args); if (!radius) radius = 5;
    lcap = pk_local_capsule();
    if (!lcap) { chat_say("Метка: не нашёл вашу капсулу (подробности в near.log)"); dlog("нативная /pk: нет капсулы игрока"); return; }
    if (!pk_pos(lcap, me)) { chat_say("Метка: не удалось прочитать вашу позицию"); dlog("нативная /pk: нет позиции игрока"); return; }
    if (!pk_collect()) { chat_say("Метка: список пикапов пуст или недоступен"); dlog("нативная /pk: Pools.Pickups пуст/недоступен"); return; }
    dlog_i("нативная /pk: пикапов в мире у клиента ", g_pk_n);
    for (i = 0; i < g_pk_n; i++) {
        float dx, dy, dz, d2;
        if (!pk_pos(g_pk_list[i], p)) { failed++; continue; }
        dx = p[0] - me[0]; dy = p[1] - me[1]; dz = p[2] - me[2]; d2 = dx * dx + dy * dy + dz * dz;
        if (allIdx < 0 || d2 < allD2) { allIdx = i; allD2 = d2; }
        if (d2 > (float)(radius * radius)) continue;
        inRange++;
        if (best < 0 || d2 < bestd2) { best = i; bestd2 = d2; }
    }
    if (best < 0) {
        char msg[300]; Buf x; b_init(&x, msg, sizeof msg);
        b_s(&x, "Метка: в радиусе "); b_int(&x, radius); b_s(&x, " м нет пикапов (в клиенте их "); b_int(&x, g_pk_n);
        b_s(&x, ", позицию не прочитал у "); b_int(&x, failed); b_s(&x, ")");
        if (allIdx >= 0) {
            int dd = 0, k; for (k = 0; k < 2000 && (float)((k + 1) * (k + 1)) <= allD2; k++) dd = k + 1;
            b_s(&x, ". Ближайший №"); b_int(&x, int_field(g_pk_list[allIdx], "m_ID", NULL, NULL, -1)); b_s(&x, " в "); b_int(&x, dd); b_s(&x, " м");
        }
        dlog_i("нативная /pk: в радиусе пусто; пикапов ", g_pk_n); dlog_i("нативная /pk:    не прочитано позиций ", failed);
        chat_say(msg); return;
    }
    bestObj = g_pk_list[best];
    if (!g_pk_mTrig || g_pk_trigCls != A.object_get_class(bestObj)) {
        g_pk_trigCls = A.object_get_class(bestObj);
        g_pk_mTrig = find_method(g_pk_trigCls, "OnTriggerEnter", 1);
    }
    if (!g_pk_mTrig) { chat_say("Метка: у пикапа нет OnTriggerEnter (версия клиента изменилась?)"); dlog("нативная /pk: Pickup.OnTriggerEnter не найден"); return; }
    {
        long id = int_field(bestObj, "m_ID", NULL, NULL, -1);
        int d10 = 0; { float d = 0; int k; for (k = 0; k < 400 && (float)((k + 1) * (k + 1)) <= bestd2 * 100.0f; k++) d = (float)(k + 1); d10 = (int)d; }
        dlog_i("нативная /pk: пикап №", id); dlog_i("нативная /pk:    расстояние (дм) ", d10); dlog_i("нативная /pk:    пикапов в радиусе ", inRange);
        a[0] = lcap;
        A.runtime_invoke(g_pk_mTrig, bestObj, a, &exc);
        if (exc) {
            char et[200], msg[300]; Buf x; exc_text(exc, et, sizeof et); dlog2("нативная /pk: OnTriggerEnter бросил: ", et);
            b_init(&x, msg, sizeof msg); b_s(&x, "Метка: вызов не прошёл — "); b_s(&x, et); chat_say(msg); return;
        }
        {
            char msg[300]; Buf x; b_init(&x, msg, sizeof msg);
            b_s(&x, "Метка: повторный вход отправлен — пикап №"); b_int(&x, id); b_s(&x, ", ");
            b_int(&x, d10 / 10); b_ch(&x, '.'); b_int(&x, d10 % 10); b_s(&x, " м");
            if (inRange > 1) { b_s(&x, " (ближайший из "); b_int(&x, inRange); b_s(&x, ")"); }
            chat_say(msg);
        }
    }
}

/* ───────────── самозапуск ───────────── */
static void pk_register(void* dict, void* mset, void* kls, int size, int off_mp, int off_ii) {
    void* nd = clone_cmd_delegate(kls, size, off_mp, off_ii, (uptr)native_pk_cmd);
    if (nd && (cmd_put(dict, mset, "/pk", nd) & cmd_put(dict, mset, "pk", nd))) dlog("нативная /pk: зарегистрирована («/pk» и «pk»)");
    else dlog("нативная /pk: НЕ зарегистрирована");
}
static int pk_boot_once(void) {
    void *cw, *fd, *dict = NULL, *kls, *mset, *f_mp, *f_ii; int size, off_mp, off_ii;
    if (!g_tplDelegate || !g_img) { dlog("нативная /pk: нет образца делегата чат-команды"); return 0; }
    ensure_attached();
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    fd = cw ? A.class_get_field_from_name(cw, "m_ChatCommandEvents") : NULL;
    if (!fd) { dlog("нативная /pk: поле WebViewSystem.m_ChatCommandEvents не найдено"); return 0; }
    A.field_static_get_value(fd, &dict);
    if (!dict) { dlog("нативная /pk: реестр чат-команд не найден"); return 0; }
    kls = A.object_get_class(g_tplDelegate);
    size = A.class_instance_size(kls);
    f_mp = find_field(kls, "method_ptr");
    if (size < 0x40 || size > 0x200 || !f_mp) { dlog("нативная /pk: странный делегат чат-команды"); return 0; }
    off_mp = A.field_get_offset(f_mp);
    f_ii = find_field(kls, "invoke_impl");
    off_ii = f_ii ? A.field_get_offset(f_ii) : 0;
    mset = find_method(A.object_get_class(dict), "set_Item", 2);
    if (!mset) { dlog("нативная /pk: у реестра нет set_Item"); return 0; }
    near_reg_lock(); pk_register(dict, mset, kls, size, off_mp, off_ii); near_reg_unlock();
    return 1;
}
static void* pk_main(void* arg) {
    int w;
    (void)arg;
    for (w = 0; !__atomic_load_n(&g_started, __ATOMIC_ACQUIRE); w++) usleep(500000);
    if (off_switch()) return NULL;
    for (w = 0; !__atomic_load_n(&g_cmdDone, __ATOMIC_ACQUIRE); w++) usleep(500000);
    if (__atomic_load_n(&g_cmdDone, __ATOMIC_ACQUIRE) != 1) { dlog("нативная /pk: Near не зарегистрировал команды, /pk не будет"); return NULL; }
    usleep(700000);                                         /* пусть сначала допишет свои команды Near (и /nocol) */
    dlog("нативная /pk: Near готов, регистрирую");
    pk_boot_once();
    return NULL;
}
#ifndef NEAR_TEST
__attribute__((constructor)) static void pk_ctor(void) {
    unsigned long t = 0;
    pthread_create(&t, NULL, pk_main, NULL);
}
#endif
