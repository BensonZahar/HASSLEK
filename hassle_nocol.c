/*
 * hassle_nocol.h — необязательный модуль «/nocol»: проходить сквозь игроков и NPC. Только на вашем клиенте, сервер ничего не знает.
 *
 * hassle_near.c НЕ МЕНЯЕТСЯ вообще. Этот файл включает его целиком (#include "hassle_near.c") и добавляет /nocol сверху,
 * сам запускает свой поток и сам регистрирует команду. build_near.py собирает hassle_nocol.c, если он лежит рядом, иначе hassle_near.c.
 * КАК УБРАТЬ: удалите hassle_nocol.c и пересоберите .so — всё как раньше, больше ничего править не нужно.
 *
 * Команда в чате (нативная, как /rec):
 *   /nocol            переключить вкл ↔ выкл
 *   /nocol on|off     включить / вернуть коллизию
 *   /nocol auto       включить и сам следить за новыми игроками/NPC (по умолчанию после «on»)
 *   /nocol manual     включить без слежки (новых добавлять повторным /nocol on)
 *   /nocol status     состояние
 *
 * Как это работает
 *   • Цели: игроки — PlayerPool.PlayersInStream, NPC — Pools.Actors (RPC Hassle.Global.NPC создаёт Actor'ов).
 *     Капсула цели: Actor.m_Controller (ThirdPersonCharacter) → Character.Movement (ICharacterMovement) → _capsuleCollider / m_Collider.
 *     ВАЖНО: Character.SimpleMovement — это bool, как объект его читать нельзя (так уже падало), поэтому все поля читаем через ref_field_s с проверкой типа.
 *   • Эффект: у вашего движения зовём IgnoreCollision(Collider, true) + Physics.IgnoreCollision (и обратно — у движения цели).
 *     По слою нельзя: у игры нет слоя «персонажи», капсулы лежат на Default вместе с картой.
 *   • Вызовы Unity/Physics делаются ТОЛЬКО в главном потоке. Авто-режим: фоновый поток (сборщик мода) каждые 300 мс читает
 *     «подпись» набора целей (только управляемые поля, как и остальной мод); если она изменилась (пришёл/ушёл игрок или NPC,
 *     вы возродились) — ставит задачу в UnitySynchronizationContext главного потока (Post). Колбэк — тот же нативный обработчик
 *     команды; делегат клонируется с готового образца чат-команды (как /rec), класс — SendOrPostCallback.
 *   • Если на момент применения коллайдер цели ещё неактивен (NPC только появился), его пропускаем и повторяем до 5 раз с паузой 2 с.
 */
#include "hassle_near.c"

#ifndef NC_NOW
#define NC_NOW() time(NULL)
#endif
#define NC_MAXT 640

/* ───────────── дополнительные функции IL2CPP (свои, hassle_near.c не трогаем) ───────────── */
static struct {
    void* (*class_get_methods)(void*, void**);
    const char* (*method_get_name)(void*);
    unsigned (*method_get_param_count)(void*);
    void* (*method_get_param)(void*, unsigned);
} NCA;
static int g_nc_apiTried;
static int nc_api(void) {
    if (!g_nc_apiTried) {
        g_nc_apiTried = 1;
        if (g_il2cpp) {
            *(void**)&NCA.class_get_methods = dlsym(g_il2cpp, "il2cpp_class_get_methods");
            *(void**)&NCA.method_get_name = dlsym(g_il2cpp, "il2cpp_method_get_name");
            *(void**)&NCA.method_get_param_count = dlsym(g_il2cpp, "il2cpp_method_get_param_count");
            *(void**)&NCA.method_get_param = dlsym(g_il2cpp, "il2cpp_method_get_param");
        }
    }
    return NCA.class_get_methods && NCA.method_get_name && NCA.method_get_param_count && NCA.method_get_param;
}

/* ───────────── безопасные помощники ───────────── */
/* Ссылочное поле С ПРОВЕРКОЙ ТИПА: bool/число/структуру как указатель не читаем. */
static void* ref_field_s(void* obj, const char* name) {
    void* f; int tk;
    if (!obj) return NULL;
    f = find_field(A.object_get_class(obj), name);
    if (!f) return NULL;
    tk = A.type_get_type(A.field_get_type(f));
    if (!(tk == 0x12 || tk == 0x15 || tk == 0x1c || tk == 0x0e || tk == 0x1d || tk == 0x14)) return NULL;   /* CLASS, GENERICINST, OBJECT, STRING, SZARRAY, ARRAY */
    return *(void**)((char*)obj + A.field_get_offset(f));
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
/* метод по имени, числу параметров и суффиксу типа первого параметра (у движения две перегрузки IgnoreCollision(Collider|Rigidbody, bool)) */
static void* find_method_p(void* k, const char* name, int argc, const char* p0suffix) {
    for (; k; k = A.class_get_parent(k)) {
        void* it = NULL; void* m;
        while ((m = NCA.class_get_methods(k, &it))) {
            const char* mn = NCA.method_get_name(m); void* t; const char* tn;
            if (!mn || !seq(mn, name)) continue;
            if ((int)NCA.method_get_param_count(m) != argc) continue;
            if (!p0suffix) return m;
            t = NCA.method_get_param(m, 0);
            tn = t ? A.type_get_name(t) : NULL;
            if (tn && ends_with(tn, p0suffix)) return m;
        }
    }
    return NULL;
}
/* обход List<T> и Dictionary<int,T> (без шума в журнале; других коллекций здесь нет) */
static int nc_walk(void* coll, item_cb cb, void* ctx) {
    const char* nm; void* arr;
    if (!coll) return 0;
    nm = A.class_get_name(A.object_get_class(coll));
    if (sprefix(nm, "List`1")) {
        arr = ref_field_s(coll, "_items");
        walk_ref_array(arr, int_field(coll, "_size", NULL, NULL, -1), 1, cb, ctx);
        return 1;
    }
    if (sprefix(nm, "Dictionary`2")) {
        arr = ref_field_s(coll, "_entries");
        if (arr) { walk_struct_array(arr, int_field(coll, "_count", NULL, NULL, -1), "value", cb, ctx); return 1; }
    }
    return 0;
}
/* Pools.Actors (static-поле, либо поле экземпляра Pools) */
static void* nc_actor_pool(void) {
    void* f; void* v = NULL;
    if (!g_cPools) return NULL;
    f = A.class_get_field_from_name(g_cPools, "Actors");
    if (!f) return NULL;
    if (A.field_get_flags(f) & 0x10) { A.field_static_get_value(f, &v); return v; }
    if (g_poolSrcField && g_poolSrcIsPools) { A.field_static_get_value(g_poolSrcField, &v); return ref_field_s(v, "Actors"); }
    return NULL;
}
/* компонент движения актёра: m_Controller → Movement (ECM2 CharacterMovement либо SimpleCharacterMovement) */
static void* nc_movement(void* actor) {
    void* ctrl = ref_field_s(actor, "m_Controller");
    if (!ctrl) return NULL;
    return ref_field_s(ctrl, "Movement");
}
static void* nc_capsule(void* mv) {
    void* c;
    if (!mv) return NULL;
    c = ref_field_s(mv, "_capsuleCollider");           /* ECM2 CharacterMovement */
    if (!c) c = ref_field_s(mv, "m_Collider");         /* SimpleCharacterMovement */
    return c;
}

/* ───────────── состояние ───────────── */
static volatile int g_nc_on;                       /* команда включена */
static volatile int g_nc_auto = 1;                 /* следить за новыми целями */
static volatile int g_nc_pending;                  /* задача уже поставлена в главный поток */
static volatile long g_nc_postT, g_nc_applyT;
static volatile uptr g_nc_sig;                     /* подпись набора целей на момент последнего применения */
static volatile int g_nc_skipped, g_nc_retry;
static int g_nc_lastP, g_nc_lastA, g_nc_lastC;
static void* g_nc_t[NC_MAXT];
static int g_nc_nt;
static void* g_nc_me;
static int g_nc_logged, g_nc_physTried, g_nc_ctrlDumped;
static void* g_mPhysIgnore;                        /* Physics.IgnoreCollision(Collider, Collider, bool) */
static void* g_nc_cacheCls[6]; static void* g_nc_cacheM[6]; static int g_nc_cacheN;
/* главный поток и делегат */
static void* g_nc_ctx;                             /* UnitySynchronizationContext главного потока */
static void* g_nc_postM;                           /* его Post(SendOrPostCallback, object) */
static void* g_nc_kCb, *g_nc_kTpl;                 /* класс делегата для колбэка / класс образца чат-команды */
static int g_nc_dsize, g_nc_offmp, g_nc_offii, g_nc_ctxTries;

typedef struct { int players, actors, cols, skipped, ign_ok, ign_exc, phys_ok, phys_exc; char err[160]; char exc[240]; } NcRes;
static void nc_err(NcRes* r, const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof r->err - 1; i++) r->err[i] = why[i];
    r->err[i] = 0;
    dlog2("нативная /nocol: отказ: ", r->err);
}
static void native_nocol_cmd(void* self, void* args, void* mi);

static void* nc_ign_method(void* obj) {
    void* k = A.object_get_class(obj); void* m; int i;
    for (i = 0; i < g_nc_cacheN; i++) if (g_nc_cacheCls[i] == k) return g_nc_cacheM[i];
    m = find_method_p(k, "IgnoreCollision", 2, ".Collider");
    if (g_nc_cacheN < 6) { g_nc_cacheCls[g_nc_cacheN] = k; g_nc_cacheM[g_nc_cacheN] = m; g_nc_cacheN++; }
    return m;
}
/* коллайдер жив и его объект активен (Physics.IgnoreCollision на неактивных ругается ошибкой в логе Unity). ТОЛЬКО главный поток. */
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

/* ───────────── «подпись» набора целей (только управляемые поля — можно звать из фонового потока) ───────────── */
typedef struct { uptr sum, xr; int n; void* me; } NcSig;
static void nc_sig_cb(void* item, void* ctx) {
    NcSig* s = (NcSig*)ctx; void* c;
    if (!item || item == s->me) return;
    c = nc_capsule(nc_movement(item));
    s->n++;
    if (c) { s->sum += ((uptr)c >> 3) * 2654435761u; s->xr ^= (uptr)c; }
}
static uptr nc_sig(void) {
    NcSig s; void *pool, *me, *mv, *stream, *apool, *items;
    pool = current_pool();
    if (!pool) return 0;
    me = ref_field_s(pool, "LocalPlayer");
    if (!me) return 0;
    s.sum = 0; s.xr = 0; s.n = 0; s.me = me;
    mv = nc_movement(me);
    s.sum += (uptr)mv * 31; s.xr ^= (uptr)nc_capsule(mv);          /* возрождение/смена модели меняет ваше движение — подпись тоже */
    stream = ref_field_s(pool, "PlayersInStream");
    if (stream) nc_walk(stream, nc_sig_cb, &s);
    apool = nc_actor_pool();
    if (apool) { items = ref_field_s(apool, "m_Items"); if (items) nc_walk(items, nc_sig_cb, &s); }
    return (s.sum ^ (s.xr << 1) ^ (uptr)(s.n * 7919)) | 1;
}

/* ───────────── применить (ТОЛЬКО главный поток) ───────────── */
static void nc_apply(int ignore, NcRes* r) {
    void *pool, *stream, *apool, *items, *lmv, *lcap, *lign;
    int t, n0; u8 ign = ignore ? 1 : 0; uptr sig0;
    r->players = r->actors = r->cols = r->skipped = r->ign_ok = r->ign_exc = r->phys_ok = r->phys_exc = 0;
    r->err[0] = 0; r->exc[0] = 0;
    if (!g_safe) { nc_err(r, "мод Near ещё стартует, подождите"); return; }
    if (!il2cpp_light()) { nc_err(r, "IL2CPP ещё не готов"); return; }
    ensure_attached();
    if (!nc_api()) { nc_err(r, "в libil2cpp нет il2cpp_class_get_methods / method_get_param"); return; }
    if (!g_poolSrcField) { nc_err(r, "пул игроков ещё не найден, подождите пару секунд"); return; }
    pool = current_pool();
    if (!pool) { nc_err(r, "вы не в игре (пул игроков не создан)"); return; }
    g_nc_me = ref_field_s(pool, "LocalPlayer");
    if (!g_nc_me) { nc_err(r, "ваш персонаж ещё не создан"); return; }
    lmv = nc_movement(g_nc_me);
    if (!lmv) {
        nc_err(r, "у вашего персонажа не найден компонент движения (подробности в near.log)");
        if (!g_nc_ctrlDumped) {
            void* ctrl = ref_field_s(g_nc_me, "m_Controller");
            g_nc_ctrlDumped = 1;
            dump_class(ctrl ? A.object_get_class(ctrl) : A.object_get_class(g_nc_me), ctrl ? "контроллер игрока (Character)" : "LocalPlayer (нет m_Controller)");
        }
        return;
    }
    dlog2("нативная /nocol: шаг 1: движение игрока найдено: ", A.class_get_name(A.object_get_class(lmv)));
    lcap = nc_capsule(lmv);
    dlog2("нативная /nocol: шаг 2: капсула игрока: ", lcap ? "найдена" : "НЕ найдена");
    lign = nc_ign_method(lmv);
    if (!g_nc_physTried) {
        void* img = find_image_pre("UnityEngine.PhysicsModule");
        void* kp = img ? A.class_from_name(img, "UnityEngine", "Physics") : NULL;
        g_nc_physTried = 1;
        if (kp) g_mPhysIgnore = A.class_get_method_from_name(kp, "IgnoreCollision", 3);
        dlog2("нативная /nocol: Physics.IgnoreCollision ", g_mPhysIgnore ? "найден" : "НЕ найден (работаю только через IgnoreCollision движения)");
    }
    if (!g_nc_logged) {
        g_nc_logged = 1;
        dlog2("нативная /nocol: IgnoreCollision(Collider,bool) у вашего движения: ", lign ? "найден" : "НЕ найден");
    }
    sig0 = nc_sig();                                   /* ДО обхода: кто придёт во время обхода — вызовет повтор */

    g_nc_nt = 0;
    stream = ref_field_s(pool, "PlayersInStream");
    if (stream) nc_walk(stream, nc_collect_cb, NULL);
    r->players = g_nc_nt; n0 = g_nc_nt;
    apool = nc_actor_pool();
    if (apool) { items = ref_field_s(apool, "m_Items"); if (items) nc_walk(items, nc_collect_cb, NULL); }
    r->actors = g_nc_nt - n0;
    dlog_i("нативная /nocol: шаг 3: собрано целей (игроки) ", r->players);
    dlog_i("нативная /nocol: шаг 3: собрано целей (NPC) ", r->actors);

    for (t = 0; t < g_nc_nt; t++) {
        void* tm = nc_movement(g_nc_t[t]); void* c; void* tign;
        if (!tm) continue;
        c = nc_capsule(tm);
        if (!c || c == lcap) continue;
        if (!nc_active(c)) { r->skipped++; continue; }
        r->cols++;
        if (lign) nc_call_ign(lign, lmv, c, ign, r);                 /* ваше движение игнорирует чужую капсулу */
        nc_call_phys(lcap, c, ign, r);                               /* и на уровне физики Unity */
        tign = nc_ign_method(tm);                                    /* обратно: движение цели игнорирует вашу капсулу (если она сама толкает вас) */
        if (tign && lcap) nc_call_ign(tign, tm, lcap, ign, r);
    }
    if (sig0) g_nc_sig = sig0;
    g_nc_applyT = NC_NOW(); g_nc_skipped = r->skipped;
    {
        char t2[330]; Buf x; b_init(&x, t2, sizeof t2);
        b_s(&x, "нативная /nocol: "); b_s(&x, ign ? "ВКЛ" : "ВЫКЛ"); b_s(&x, ": игроков "); b_int(&x, r->players);
        b_s(&x, ", NPC "); b_int(&x, r->actors); b_s(&x, ", капсул "); b_int(&x, r->cols); b_s(&x, ", неактивных "); b_int(&x, r->skipped);
        b_s(&x, ", IgnoreCollision ок/ошибок "); b_int(&x, r->ign_ok); b_ch(&x, '/'); b_int(&x, r->ign_exc);
        b_s(&x, ", Physics ок/ошибок "); b_int(&x, r->phys_ok); b_ch(&x, '/'); b_int(&x, r->phys_exc);
        dlog(t2);
        if (r->exc[0]) dlog2("нативная /nocol: первое исключение: ", r->exc);
    }
}

/* ───────────── главный поток по заказу из фонового (UnitySynchronizationContext.Post) ───────────── */
static void nc_capture_ctx(void) {                  /* вызывать в главном потоке: SynchronizationContext.Current потокозависим */
    void *img, *k, *m, *ctx, *exc = NULL, *kcb; const char* cn;
    if (g_nc_ctx || g_nc_ctxTries >= 3) return;
    g_nc_ctxTries++;
    img = find_image_pre("mscorlib");
    k = img ? A.class_from_name(img, "System.Threading", "SynchronizationContext") : NULL;
    m = k ? A.class_get_method_from_name(k, "get_Current", 0) : NULL;
    if (!m) { dlog("нативная /nocol: авто: нет SynchronizationContext.get_Current"); return; }
    ctx = A.runtime_invoke(m, NULL, NULL, &exc);
    if (exc || !ctx) { dlog("нативная /nocol: авто: SynchronizationContext.Current недоступен"); return; }
    cn = A.class_get_name(A.object_get_class(ctx));
    if (!seq(cn, "UnitySynchronizationContext")) { dlog2("нативная /nocol: авто: контекст не Unity, не используем: ", cn); return; }   /* иначе Post ушёл бы в пул потоков */
    m = find_method(A.object_get_class(ctx), "Post", 2);
    if (!m) { dlog("нативная /nocol: авто: у контекста нет Post"); return; }
    kcb = A.class_from_name(img, "System.Threading", "SendOrPostCallback");
    g_nc_kCb = (kcb && A.class_instance_size && A.class_instance_size(kcb) == g_nc_dsize) ? kcb : g_nc_kTpl;
    g_nc_postM = m; g_nc_ctx = ctx;
    dlog2("нативная /nocol: авто: главный поток захвачен, делегат: ", A.class_get_name(g_nc_kCb));
}
static void nc_post(void) {                         /* из фонового потока: поставить задачу «применить» в главный */
    void *cb, *st, *args[2], *exc = NULL; char et[200];
    cb = clone_cmd_delegate(g_nc_kCb, g_nc_dsize, g_nc_offmp, g_nc_offii, (uptr)native_nocol_cmd);
    st = A.string_new("nocol-auto");
    if (!cb || !st) { g_nc_auto = 0; dlog("нативная /nocol: авто: не создался делегат, авто выключено"); return; }
    args[0] = cb; args[1] = st;
    A.runtime_invoke(g_nc_postM, g_nc_ctx, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("нативная /nocol: авто: Post бросил, авто выключено: ", et); g_nc_auto = 0; return; }
    g_nc_pending = 1; g_nc_postT = NC_NOW();
}
/* поток-сборщик, каждые ~300 мс. Только чтение управляемых полей + Post. */
static void nocol_bg_tick(void) {
    long now; uptr sig; int want = 0;
    if (!g_nc_on || !g_nc_auto || !g_nc_ctx || !g_nc_postM || !g_nc_kCb) return;
    if (!g_safe || !g_il2cpp || !g_img || !g_poolSrcField) return;
    now = NC_NOW();
    if (g_nc_pending) { if (now - g_nc_postT < 5) return; g_nc_pending = 0; }      /* колбэк так и не отработал — пробуем снова */
    if (now - g_nc_postT < 1) return;
    sig = nc_sig();
    if (!sig) return;
    if (sig != g_nc_sig) { g_nc_retry = 0; want = 1; }                              /* состав целей изменился */
    else if (g_nc_skipped > 0 && g_nc_retry < 5 && now - g_nc_applyT >= 2) { g_nc_retry++; want = 1; }   /* были неактивные капсулы — повторим */
    if (want) nc_post();
}
static void nc_auto_apply(void) {                   /* главный поток, из Post */
    NcRes r;
    g_nc_pending = 0;
    if (!g_nc_on || !g_nc_auto) return;
    nc_apply(1, &r);
    if (r.err[0]) { g_nc_postT = NC_NOW() + 4; return; }                            /* ошибка — пауза ~5 с, чтобы не спамить */
    g_nc_lastP = r.players; g_nc_lastA = r.actors; g_nc_lastC = r.cols;
}

/* ───────────── команда ───────────── */
/* слова из аргументов (по пробелам, без учёта регистра): 1 on/1, 2 off/0, 8 status/diag, 16 auto, 32 manual */
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
            else if (seq(tok, "auto")) w |= 16;
            else if (seq(tok, "manual")) w |= 32;
        }
    }
    return w;
}

static void native_nocol_cmd(void* self, void* args, void* mi) {
    int w, want; NcRes r; Buf b; char msg[460];
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { nc_auto_apply(); return; }   /* вызов из Post (state — строка), не из чата (string[]) */
    w = nc_parse(args);
    dlog_i("нативная /nocol: вызвана, флаги ", w);
    b_init(&b, msg, sizeof msg);
    if (w & 8) {
        b_s(&b, "Коллизия с игроками и NPC: "); b_s(&b, g_nc_on ? "ВЫКЛЮЧЕНА" : "включена (как в игре)");
        if (g_nc_on) {
            b_s(&b, " — игроков "); b_int(&b, g_nc_lastP); b_s(&b, ", NPC "); b_int(&b, g_nc_lastA); b_s(&b, ", капсул "); b_int(&b, g_nc_lastC);
            b_s(&b, g_nc_auto ? (g_nc_ctx ? "; авто: да" : "; авто: нет (повторите /nocol on)") : "; авто: выключено (manual)");
        }
        chat_say(msg);
        return;
    }
    if (w & 16) g_nc_auto = 1;
    if (w & 32) g_nc_auto = 0;
    want = (w & 2) ? 0 : (w & (1 | 16 | 32)) ? 1 : !g_nc_on;
    if (want) nc_capture_ctx();                          /* главный поток: берём его SynchronizationContext для авто-режима */
    nc_apply(want, &r);
    if (r.err[0]) { b_s(&b, "Коллизия: не вышло — "); b_s(&b, r.err); chat_say(msg); return; }
    if (r.cols > 0 && r.ign_ok == 0 && r.phys_ok == 0) {
        b_s(&b, "Коллизия: вызов не прошёл ("); b_int(&b, r.ign_exc); b_s(&b, " ошибок), подробности в HLOG/near.log");
        chat_say(msg);
        return;
    }
    g_nc_on = want; g_nc_lastP = r.players; g_nc_lastA = r.actors; g_nc_lastC = r.cols;
    if (!want) { b_s(&b, "Коллизия с игроками и NPC включена обратно"); chat_say(msg); return; }
    if (r.cols == 0 && r.skipped == 0 && !(g_nc_auto && g_nc_ctx)) {
        b_s(&b, "Коллизия: рядом нет ни игроков, ни NPC. Когда появятся — снова /nocol on");
        chat_say(msg);
        return;
    }
    b_s(&b, "Коллизия с игроками и NPC ВЫКЛЮЧЕНА: игроков "); b_int(&b, r.players); b_s(&b, ", NPC "); b_int(&b, r.actors);
    if (g_nc_auto && g_nc_ctx) b_s(&b, ". Авто: новых подхватываю сам");
    else if (g_nc_auto) b_s(&b, ". Авто недоступно (см. near.log), новых — повторным /nocol on");
    else b_s(&b, ". Ручной режим: новых — повторным /nocol on");
    b_s(&b, ". Вернуть: /nocol off");
    chat_say(msg);
}

/* регистрация в реестре чат-команд (вызывает наш поток nocol_boot_once ниже) */
static void nocol_register(void* dict, void* mset, void* kls, int size, int off_mp, int off_ii) {
    void* nd = clone_cmd_delegate(kls, size, off_mp, off_ii, (uptr)native_nocol_cmd);
    g_nc_kTpl = kls; g_nc_dsize = size; g_nc_offmp = off_mp; g_nc_offii = off_ii;
    if (nd && (cmd_put(dict, mset, "/nocol", nd) & cmd_put(dict, mset, "nocol", nd))) dlog("нативная /nocol: зарегистрирована («/nocol» и «nocol»)");
    else dlog("нативная /nocol: НЕ зарегистрирована");
}

/* ───────────── самозапуск (hassle_near.c об этом ничего не знает) ───────────── */
/* Ждёт, пока мод Near сам зарегистрирует /rec (значит, игра готова, а образец делегата чат-команды найден), берёт тот же реестр и
   образец и добавляет /nocol. Потом в том же потоке каждые 300 мс зовёт слежку за новыми игроками/NPC. */
static int nocol_boot_once(void) {
    void *cw, *fd, *dict = NULL, *kls, *mset, *f_mp, *f_ii; int size, off_mp, off_ii;
    if (!g_tplDelegate || !g_img) { dlog("нативная /nocol: нет образца делегата чат-команды"); return 0; }
    ensure_attached();
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    fd = cw ? A.class_get_field_from_name(cw, "m_ChatCommandEvents") : NULL;
    if (!fd) { dlog("нативная /nocol: поле WebViewSystem.m_ChatCommandEvents не найдено"); return 0; }
    A.field_static_get_value(fd, &dict);
    if (!dict) { dlog("нативная /nocol: реестр чат-команд не найден"); return 0; }
    kls = A.object_get_class(g_tplDelegate);
    size = A.class_instance_size(kls);
    f_mp = find_field(kls, "method_ptr");
    if (size < 0x40 || size > 0x200 || !f_mp) { dlog("нативная /nocol: странный делегат чат-команды"); return 0; }
    off_mp = A.field_get_offset(f_mp);
    f_ii = find_field(kls, "invoke_impl");
    off_ii = f_ii ? A.field_get_offset(f_ii) : 0;
    mset = find_method(A.object_get_class(dict), "set_Item", 2);
    if (!mset) { dlog("нативная /nocol: у реестра нет set_Item"); return 0; }
    nocol_register(dict, mset, kls, size, off_mp, off_ii);
    return 1;
}
static void* nocol_main(void* arg) {
    int w;
    (void)arg;
    for (w = 0; !__atomic_load_n(&g_started, __ATOMIC_ACQUIRE); w++) usleep(500000);      /* ждём, пока Near стартует (JNI_OnLoad) */
    if (off_switch()) return NULL;                                                      /* near_off — мод выключен целиком */
    for (w = 0; !__atomic_load_n(&g_cmdDone, __ATOMIC_ACQUIRE); w++) usleep(500000);      /* 1 — /rec зарегистрирована, -1 — Near сдался */
    if (__atomic_load_n(&g_cmdDone, __ATOMIC_ACQUIRE) != 1) { dlog("нативная /nocol: Near не зарегистрировал команды, /nocol не будет"); return NULL; }
    dlog("нативная /nocol: Near готов, регистрирую");
    if (!nocol_boot_once()) return NULL;
    for (;;) { nocol_bg_tick(); usleep(TICK_US); }
    return NULL;
}
#ifndef NEAR_TEST
__attribute__((constructor)) static void nocol_ctor(void) {   /* только создаёт поток-ждуна; до готовности игры он ничего в IL2CPP не трогает */
    unsigned long t = 0;
    pthread_create(&t, NULL, nocol_main, NULL);
}
#endif
