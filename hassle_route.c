/*
 * hassle_route.c — необязательный модуль «/route»: записать свой маршрут пешком и повторить его так же.
 *
 * hassle_near.c и hassle_nocol.c НЕ МЕНЯЮТСЯ. Этот файл включает hassle_nocol.c (а тот — hassle_near.c), сам запускает свой поток
 * и сам регистрирует команду. build_near.py собирает hassle_route.c, если он лежит рядом.
 * КАК УБРАТЬ: удалите hassle_route.c и пересоберите .so — всё как раньше.
 *
 * Команда в чате (нативная, как /rec и /nocol):
 *   /route rec            начать запись; запись стартует, когда вы возьмётесь за левый стик
 *   /route stop           остановить запись (или остановить повтор); запись сохраняется в HLOG/route.txt
 *   /route play           повторить записанный маршрут (без записи в памяти — берёт HLOG/route.txt)
 *   /route play force     то же, но без проверки «вы стоите там же, где начинали запись»
 *   /route play nocam     повтор без выставления камеры (камеру держите сами)
 *   /route status         состояние
 *   /route probe          найти экранный стик и показать его значение (диагностика)
 *
 * Как это устроено (по дампу игры и по Hud.js):
 *   • Экранный стик рисует Vue-компонент в HUD. Он НЕ связан с VirtualControlsManager (тот, что нашёл прошлый вариант мода: ключи
 *     Horizontal/Vertical/Joystick, m_Pressed всегда 0). Компонент зовёт window.onScreenControlTouchStart/Move/End("<Gamepad>/leftStick", x, y),
 *     это Hassle.WebView.Controls.OnScreenControls → OnWebViewScreenStick (OnScreenControl из Unity Input System), который пишет
 *     значение в виртуальный <Gamepad>/leftStick. Игра читает движение уже оттуда.
 *   • Объект стика лежит в статическом словаре OnScreenControls.m_Controls (ключ "<Gamepad>/leftStick") и существует, пока HUD на экране
 *     (плавающий круг рисуется только при касании, а сам стик есть всегда).
 *   • Запись: КАЖДЫЙ КАДР (в главном потоке) читаем значение стика InputControl<Vector2>.ReadUnprocessedValue() и пишем, когда оно изменилось.
 *   • Повтор: каждый кадр зовём OnWebViewScreenStick.OnTouchStart/OnTouchMove(Vector2)/OnTouchEnd — ровно то, что делает палец через JS.
 *   • Все вызовы IL2CPP идут ТОЛЬКО из главного потока. Фоновый поток лишь раз в ~12 мс ставит в очередь SynchronizationContext.Post
 *     «тик» (механизм из /nocol), а время считает по монотонным часам, так что пропущенные кадры ничего не ломают.
 *   • Камера: Game.MainCamera.m_Aim (CinemachinePOV) → m_HorizontalAxis.Value (поворот, градусы) и m_VerticalAxis.Value (наклон).
 *     Пишется вместе со стиком; при повторе камера выставляется сначала (пауза ~0,25 с), потом повторяются и её повороты.
 *   • Если во время повтора вы возьмётесь за стик сами, повтор прерывается (значение стика не совпадает с тем, что писал мод).
 *
 * Ограничения: повторяются стик и камера. Прыжок и спринт (кнопки) НЕ записываются. Положение игрока перед /route play проверяется
 * по точке начала записи (не дальше 2 м). Во время повтора камера занята модом (/route play nocam — чтобы камеру держали вы).
 * Точность: игра идёт по кадрам, на длинном маршруте возможен небольшой снос; по окончании мод пишет, на сколько метров вы не дошли до конца записи.
 */
#include "hassle_nocol.c"

struct rt_ts { long sec; long nsec; };
extern int clock_gettime(int, void*);

#define RT_MAX 30000                 /* точек записи (на ~8 минут непрерывной ходьбы) */
#define RT_MAXJ 8
#define RT_FBUF (3 << 19)             /* 1,5 МБ: хватает на заполненную запись */
#define RT_MAXMS 600000              /* запись/повтор не длиннее 10 минут */

/* ───────────── дополнительные функции IL2CPP ───────────── */
static struct { void* (*class_get_type)(void*); void* (*type_get_object)(void*); } RA;
static int g_rt_apiTried;
static int rt_api(void) {
    if (!g_rt_apiTried) {
        g_rt_apiTried = 1;
        if (g_il2cpp) {
            *(void**)&RA.class_get_type = dlsym(g_il2cpp, "il2cpp_class_get_type");
            *(void**)&RA.type_get_object = dlsym(g_il2cpp, "il2cpp_type_get_object");
        }
    }
    return RA.class_get_type && RA.type_get_object;
}

/* ───────────── данные ───────────── */
typedef struct { unsigned t; float x, y; unsigned p; float c, v; } RtS;   /* t — мс от начала, p — стик задет (x или y не 0), x/y — значение стика, c/v — поворот/наклон камеры */
static RtS g_rt_s[RT_MAX];
static volatile int g_rt_n;
static volatile int g_rt_mode;       /* 0 — ничего, 1 — запись, 2 — повтор */
static volatile int g_rt_tickPending;
static volatile long long g_rt_tickT;
static int g_rt_idx = -1;
static char g_rt_hn[64], g_rt_vn[64];                              /* в файле остались для совместимости формата */
static int g_rt_hasS, g_rt_hasE;
static float g_rt_S[3], g_rt_E[3];

#define RT_STICK "<Gamepad>/leftStick"
static void* g_rt_st;  static void* g_rt_stk;                      /* OnWebViewScreenStick и его заголовок (проверка перед вызовом) */
static void* g_rt_ctrl; static void* g_rt_ctrlk;                   /* InputControl этого стика (читаем значение) */
static void *g_rt_mRead, *g_rt_mTS, *g_rt_mTM, *g_rt_mTE;          /* ReadUnprocessedValue/ReadValue, OnTouchStart, OnTouchMove(Vector2), OnTouchEnd */
static char g_rt_key[64];
static int g_rt_started;                                           /* в записи: за стик уже взялись */
static long long g_rt_t0;
static int g_rt_pi;
static int g_rt_wrote, g_rt_bad, g_rt_down, g_rt_whi;
static float g_rt_wh[4][2];                                        /* последние записанные значения (стик обновляется с задержкой в кадр) */
static float g_rt_lx, g_rt_ly;
static unsigned g_rt_lp;
static char g_rt_fbuf[RT_FBUF];
/* камера */
static void* g_rt_aim; static void* g_rt_aimk;                    /* CinemachinePOV и его заголовок (проверка перед записью) */
static int g_rt_offCH = -1, g_rt_offCV = -1;                      /* абсолютные смещения m_HorizontalAxis.Value / m_VerticalAxis.Value в объекте POV */
static int g_rt_camRec;                                           /* в записи есть камера */
static int g_rt_camOn;                                            /* при повторе выставляем камеру */
static float g_rt_lc, g_rt_lv;                                    /* последние записанные поворот/наклон */

static long long rt_now(void) {
    struct rt_ts t;
    if (clock_gettime(1, &t)) return 0;                          /* CLOCK_MONOTONIC */
    return (long long)t.sec * 1000 + t.nsec / 1000000;
}

/* ───────────── поиск экранного стика (ТОЛЬКО главный поток) ───────────── */
static void rt_str(void* s, char* out, int cap) {
    int n, i; const u16* ch;
    out[0] = 0;
    if (!s || !nc_readable(s, 24)) return;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 60 || !ch) return;
    utf16_to_utf8(ch, n, out, cap);
    for (i = 0; out[i]; i++) if (out[i] == ' ') out[i] = '_';
}

static int rt_stick_ok(void) { return g_rt_st && nc_obj_ok(g_rt_st) && *(void**)g_rt_st == g_rt_stk; }
static int rt_ctrl_ok(void) { return g_rt_ctrl && nc_obj_ok(g_rt_ctrl) && *(void**)g_rt_ctrl == g_rt_ctrlk; }

/* значение стика (главный поток). Результат — упакованный Vector2: два float сразу после заголовка объекта */
static int rt_stick_read(float* x, float* y) {
    void *r, *exc = NULL; const float* f;
    if (!g_rt_mRead || !rt_ctrl_ok()) return 0;
    r = A.runtime_invoke(g_rt_mRead, g_rt_ctrl, NULL, &exc);
    if (exc || !r || !nc_readable((char*)r + HDR, 8)) return 0;
    f = (const float*)((char*)r + HDR);
    *x = f[0]; *y = f[1];
    return 1;
}
/* what: 1 — OnTouchStart, 2 — OnTouchMove(x, y), 3 — OnTouchEnd */
static int rt_stick_send(int what, float x, float y) {
    void *a[1], *exc = NULL; float v[2]; void* m = what == 1 ? g_rt_mTS : what == 2 ? g_rt_mTM : g_rt_mTE;
    if (!m || !rt_stick_ok()) return 0;
    v[0] = x; v[1] = y; a[0] = v;                                /* Vector2 передаётся указателем на два float */
    A.runtime_invoke(m, g_rt_st, what == 2 ? a : NULL, &exc);
    return exc ? 0 : 1;
}

static int rt_find(void) {
    void *kc, *fd, *dict = NULL, *dk, *fe, *fc, *ent, *best = NULL; long cnt, i; int offE, offC, stride;
    char bestKey[64]; const char* bestCls = NULL;
    g_rt_st = g_rt_ctrl = g_rt_mRead = g_rt_mTS = g_rt_mTM = g_rt_mTE = NULL; g_rt_key[0] = 0; bestKey[0] = 0;
    if (!g_img) { dlog("маршрут: нет образа игры"); return 0; }
    kc = A.class_from_name(g_img, "Hassle.WebView.Controls", "OnScreenControls");
    if (!kc) { dlog("маршрут: класс OnScreenControls не найден"); return 0; }
    fd = A.class_get_field_from_name(kc, "m_Controls");
    if (!fd) { dlog("маршрут: у OnScreenControls нет поля m_Controls"); return 0; }
    A.field_static_get_value(fd, &dict);
    if (!dict || !nc_obj_ok(dict)) { dlog("маршрут: OnScreenControls.m_Controls = null (HUD с экранным управлением не создан?)"); return 0; }
    dk = A.object_get_class(dict);
    fe = find_field(dk, "_entries"); fc = find_field(dk, "_count");
    if (!fe || !fc) { dlog("маршрут: у словаря нет полей _entries/_count"); return 0; }
    offE = A.field_get_offset(fe); offC = A.field_get_offset(fc);
    if (offE < 0x10 || offE > 0x100 || offC < 0x10 || offC > 0x100 || !nc_readable((char*)dict + offE, sizeof(void*)) || !nc_readable((char*)dict + offC, 4)) { dlog("маршрут: странные смещения словаря"); return 0; }
    ent = *(void**)((char*)dict + offE); cnt = *(int*)((char*)dict + offC);
    stride = 8 + 2 * (int)sizeof(void*);                           /* Entry<string, ref>: int hashCode; int next; key; value */
    if (!ent || cnt < 0 || cnt > 256) { dlog_i("маршрут: словарь экранных элементов странный, count=", cnt); return 0; }
    if (cnt == 0 || !nc_range_ok(ent, 4 * (long)sizeof(void*) + cnt * stride)) { dlog("маршрут: экранных элементов нет (HUD ещё не создал стик)"); return 0; }
    for (i = 0; i < cnt; i++) {
        char* e = (char*)ent + 4 * sizeof(void*) + (uptr)i * (uptr)stride;
        void* key = *(void**)(e + 8);
        void* val = *(void**)(e + 8 + sizeof(void*));
        char k[64]; const char* cn;
        if (!val || !nc_obj_ok(val)) continue;
        rt_str(key, k, sizeof k);
        cn = A.class_get_name(A.object_get_class(val));
        dlog2("маршрут: экранный элемент: ", k);
        dlog2("маршрут:    класс: ", cn ? cn : "?");
        if (cn && seq(cn, "OnWebViewScreenStick") && (!best || seq(k, RT_STICK))) {
            best = val; bestCls = cn;
            { int q; for (q = 0; q < 63 && k[q]; q++) bestKey[q] = k[q]; bestKey[q] = 0; }
        }
    }
    if (!best) { dlog("маршрут: среди экранных элементов нет OnWebViewScreenStick"); return 0; }
    (void)bestCls;
    {
        void* kcls = A.object_get_class(best); void *ctrl, *m1, *m2; float tx, ty;
        g_rt_mTS = find_method(kcls, "OnTouchStart", 0);
        g_rt_mTM = find_method(kcls, "OnTouchMove", 1);
        g_rt_mTE = find_method(kcls, "OnTouchEnd", 0);
        if (!g_rt_mTS || !g_rt_mTM || !g_rt_mTE) { dlog("маршрут: у стика нет OnTouchStart/OnTouchMove/OnTouchEnd"); return 0; }
        ctrl = ref_field_s(best, "m_Control");
        if (!ctrl || !nc_obj_ok(ctrl)) {                           /* поле ещё пусто — спросим свойством control */
            void *gm = find_method(kcls, "get_control", 0), *exc = NULL;
            ctrl = gm ? A.runtime_invoke(gm, best, NULL, &exc) : NULL;
            if (exc) ctrl = NULL;
        }
        if (!ctrl || !nc_obj_ok(ctrl)) { dlog("маршрут: у стика нет InputControl (виртуальный геймпад ещё не создан?)"); return 0; }
        g_rt_st = best; g_rt_stk = *(void**)best; g_rt_ctrl = ctrl; g_rt_ctrlk = *(void**)ctrl;
        m1 = find_method(A.object_get_class(ctrl), "ReadUnprocessedValue", 0);
        m2 = find_method(A.object_get_class(ctrl), "ReadValue", 0);
        g_rt_mRead = m1;
        if (!m1 || !rt_stick_read(&tx, &ty)) { g_rt_mRead = m2; if (!m2 || !rt_stick_read(&tx, &ty)) { g_rt_mRead = NULL; g_rt_st = g_rt_ctrl = NULL; dlog("маршрут: значение стика не читается (ReadUnprocessedValue/ReadValue)"); return 0; } }
        { int q; for (q = 0; q < 63 && bestKey[q]; q++) g_rt_key[q] = bestKey[q]; g_rt_key[q] = 0; }
    }
    dlog2("маршрут: стик найден, ключ ", g_rt_key);
    return 1;
}

/* камера (ТОЛЬКО главный поток): Game.MainCamera (static) → m_Aim (CinemachinePOV) → AxisState.Value горизонтали и вертикали */
static int rt_cam_find(void) {
    void *fn, *cam = NULL, *aim, *ak, *fh, *fv, *sc, *fval; int sh, vo;
    g_rt_aim = NULL; g_rt_offCH = g_rt_offCV = -1;
    if (!g_cGame || !g_img) { dlog("маршрут: камера: класс Game не найден"); return 0; }
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) { dlog("маршрут: камера: Game.MainCamera не найдено"); return 0; }
    A.field_static_get_value(fn, &cam);
    if (!cam || !nc_obj_ok(cam)) { dlog("маршрут: камера: Game.MainCamera = null"); return 0; }
    aim = ref_field_s(cam, "m_Aim");
    if (!aim || !nc_obj_ok(aim)) { dlog("маршрут: камера: MainCamera.m_Aim пуст (камера не в режиме слежения?)"); return 0; }
    ak = A.object_get_class(aim);
    fh = find_field(ak, "m_HorizontalAxis"); fv = find_field(ak, "m_VerticalAxis");
    if (!fh || !fv) { dlog2("маршрут: камера: нет m_HorizontalAxis/m_VerticalAxis в классе ", A.class_get_name(ak)); return 0; }
    sc = A.class_from_type(A.field_get_type(fh));
    fval = sc ? find_field(sc, "Value") : NULL;
    if (!fval) { dlog("маршрут: камера: у AxisState нет поля Value"); return 0; }
    sh = struct_shift(sc);
    vo = A.field_get_offset(fval) - sh;
    g_rt_offCH = A.field_get_offset(fh) + vo; g_rt_offCV = A.field_get_offset(fv) + vo;
    if (g_rt_offCH < 0x10 || g_rt_offCH > 0x400 || g_rt_offCV < 0x10 || g_rt_offCV > 0x400 || !nc_readable((char*)aim + g_rt_offCH, 4) || !nc_readable((char*)aim + g_rt_offCV, 4)) {
        dlog_i("маршрут: камера: странные смещения ", g_rt_offCH); g_rt_offCH = g_rt_offCV = -1; return 0;
    }
    g_rt_aim = aim; g_rt_aimk = *(void**)aim;
    dlog_i("маршрут: камера найдена, смещение поворота ", g_rt_offCH);
    return 1;
}
static int rt_cam_read(float* c, float* v) {
    char* o = (char*)g_rt_aim;
    if (!o || g_rt_offCH < 0 || !nc_readable(o, 16) || *(void**)o != g_rt_aimk || !nc_readable(o + g_rt_offCH, 4) || !nc_readable(o + g_rt_offCV, 4)) return 0;
    *c = *(volatile float*)(o + g_rt_offCH);
    *v = *(volatile float*)(o + g_rt_offCV);
    return 1;
}
static int rt_cam_write(float c, float v) {
    char* o = (char*)g_rt_aim;
    if (!o || g_rt_offCH < 0 || !nc_readable(o, 16) || *(void**)o != g_rt_aimk || !nc_readable(o + g_rt_offCH, 4) || !nc_readable(o + g_rt_offCV, 4)) return 0;
    *(volatile float*)(o + g_rt_offCH) = c;
    *(volatile float*)(o + g_rt_offCV) = v;
    return 1;
}

/* ───────────── тик: всё происходит в главном потоке ───────────── */
static void native_route_cmd(void* self, void* args, void* mi);
static void rt_report(int code);
static void rt_event(int code) { dlog_i("маршрут: событие ", code); rt_report(code); }

static void rt_post(void) {                                        /* фоновый поток: поставить тик в очередь главного потока */
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    if (!g_nc_ctx || !g_nc_postM || !g_nc_kCb) { g_rt_tickPending = 0; return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_nc_kCb, g_nc_dsize, g_nc_offmp, g_nc_offii, (uptr)native_route_cmd);
    st = A.string_new("route-tick");
    if (!cb || !st) { g_rt_tickPending = 0; dlog("маршрут: не создался делегат для Post"); return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_nc_postM, g_nc_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("маршрут: Post бросил: ", et); g_rt_tickPending = 0; }
}

static void rt_push(unsigned t, unsigned p, float x, float y, float c, float v) {
    int n = g_rt_n;
    if (n >= RT_MAX) return;
    g_rt_s[n].t = t; g_rt_s[n].p = p; g_rt_s[n].x = x; g_rt_s[n].y = y; g_rt_s[n].c = c; g_rt_s[n].v = v;
    __atomic_store_n(&g_rt_n, n + 1, __ATOMIC_RELEASE);
}
static float rt_abs(float v) { return v < 0 ? -v : v; }

static void rt_stop_rec(Buf* b);
static void rt_release_stick(void) {                               /* отпустить стик, если мы его держали */
    if (g_rt_down) { rt_stick_send(3, 0, 0); g_rt_down = 0; }
}

static void rt_rec_step(void) {
    float x, y, c = 0, v = 0; unsigned p; long long now;
    if (g_rt_camRec && !rt_cam_read(&c, &v)) { c = g_rt_lc; v = g_rt_lv; }
    if (!rt_stick_read(&x, &y)) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(4); return; }
    p = (x != 0 || y != 0) ? 1u : 0u;
    if (!g_rt_started) {                                           /* ждём, когда вы возьмётесь за стик */
        if (p) {
            g_rt_started = 1; g_rt_t0 = rt_now();
            g_rt_lp = p; g_rt_lx = x; g_rt_ly = y; g_rt_lc = c; g_rt_lv = v;
            g_rt_n = 0;
            rt_push(0, p, x, y, c, v);
            dlog("маршрут: запись началась");
        }
        return;
    }
    now = rt_now() - g_rt_t0;
    if (p != g_rt_lp || rt_abs(x - g_rt_lx) > 0.004f || rt_abs(y - g_rt_ly) > 0.004f || (g_rt_camRec && (rt_abs(c - g_rt_lc) > 0.15f || rt_abs(v - g_rt_lv) > 0.15f))) {
        rt_push((unsigned)now, p, x, y, c, v);
        g_rt_lp = p; g_rt_lx = x; g_rt_ly = y; g_rt_lc = c; g_rt_lv = v;
    }
    if (g_rt_n >= RT_MAX - 2) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(3); return; }
    if (now > RT_MAXMS) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(5); return; }
}

static void rt_play_stop(int code) {
    __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE);
    rt_release_stick();
    if (code) rt_event(code);
}

static void rt_play_step(void) {
    long long now = rt_now() - g_rt_t0; int n = g_rt_n, i, ok; float cx, cy, wx, wy; RtS* s;
    if (n < 2) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); return; }
    if (now < 0) {                                               /* предстарт: сначала выставляем камеру в положение начала записи, стик ещё не трогаем */
        if (g_rt_camOn && !rt_cam_write(g_rt_s[0].c, g_rt_s[0].v)) { g_rt_camOn = 0; dlog("маршрут: камеру выставить не удалось, дальше только стик"); }
        return;
    }
    if (g_rt_wrote) {                                            /* кто-то (вы) изменил стик не так, как мы писали — уступаем */
        if (!rt_stick_read(&cx, &cy)) { rt_play_stop(4); return; }
        ok = 0;                                                  /* значение доходит до стика с задержкой в кадр: сверяем с несколькими последними записанными */
        for (i = 0; i < 4; i++) if (rt_abs(cx - g_rt_wh[i][0]) <= 0.02f && rt_abs(cy - g_rt_wh[i][1]) <= 0.02f) ok = 1;
        if (!ok) {
            /* значение не наше: НЕ перезаписываем (иначе будем бороться с пальцем); 6 тиков подряд — уступаем */
            if (++g_rt_bad >= 6) {
                char t[200]; Buf b; b_init(&b, t, sizeof t);
                b_s(&b, "маршрут: повтор прерван, прочитали стик "); b_fix(&b, cx, 3); b_ch(&b, '/'); b_fix(&b, cy, 3);
                dlog(t);
                rt_play_stop(2);
            }
            return;
        }
        g_rt_bad = 0;
    }
    while (g_rt_pi + 1 < n && (long long)g_rt_s[g_rt_pi + 1].t <= now) g_rt_pi++;
    s = &g_rt_s[g_rt_pi];
    if (g_rt_pi >= n - 1 && now >= (long long)g_rt_s[n - 1].t) {            /* дошли до конца записи: отпускаем стик */
        rt_play_stop(1);
        return;
    }
    if (g_rt_camOn && !rt_cam_write(s->c, s->v)) { g_rt_camOn = 0; dlog("маршрут: камера пропала (режим камеры сменился?), дальше только стик"); }
    if (s->p) {
        if (!g_rt_down) { if (!rt_stick_send(1, 0, 0)) { rt_play_stop(4); return; } g_rt_down = 1; }
        if (!rt_stick_send(2, s->x, s->y)) { rt_play_stop(4); return; }
        wx = s->x; wy = s->y;
    } else {
        if (g_rt_down) { if (!rt_stick_send(3, 0, 0)) { rt_play_stop(4); return; } g_rt_down = 0; }
        wx = 0; wy = 0;
    }
    g_rt_wh[g_rt_whi & 3][0] = wx; g_rt_wh[g_rt_whi & 3][1] = wy; g_rt_whi++;
    g_rt_wrote = 1;
}

static void rt_tick(void) {                                        /* главный поток */
    int mode = __atomic_load_n(&g_rt_mode, __ATOMIC_ACQUIRE);
    g_rt_tickPending = 0;
    if (mode == 1) rt_rec_step(); else if (mode == 2) rt_play_step();
}

static void* rt_worker(void* arg) {
    (void)arg;
    for (;;) {
        int mode = __atomic_load_n(&g_rt_mode, __ATOMIC_ACQUIRE);
        if (mode == 0) { usleep(40000); continue; }
        if (!g_rt_tickPending || rt_now() - g_rt_tickT > 1000) {   /* предыдущий тик ещё не выполнен — не копим очередь (но если потерялся — ставим заново) */
            g_rt_tickPending = 1; g_rt_tickT = rt_now();
            rt_post();
        }
        usleep(12000);
    }
    return NULL;
}

/* ───────────── файл HLOG/route.txt ─────────────
   H <имя_гориз> <имя_верт> <индекс>     (пустое имя — «-»)
   S x y z    позиция в начале записи   E x y z    позиция в конце     K 1 — в записи есть камера
   затем строки: <мс> <нажат 0/1> <x> <y> [<поворот камеры> <наклон камеры>] */
static int rt_path(char* out, int cap) {
    int i, last = -1; Buf b;
    if (!g_flog[0]) return 0;
    for (i = 0; g_flog[i]; i++) if (g_flog[i] == '/') last = i;
    if (last < 0 || last + 12 > cap) return 0;
    b_init(&b, out, cap);
    for (i = 0; i <= last; i++) b_ch(&b, g_flog[i]);
    b_s(&b, "route.txt");
    return 1;
}
static int rt_save(void) {
    char path[320]; static char buf[8192]; Buf b; int i, n = g_rt_n, first = 1;
    if (!rt_path(path, sizeof path)) return 0;
    b_init(&b, buf, sizeof buf);
    b_s(&b, "H "); b_s(&b, g_rt_hn[0] ? g_rt_hn : "-"); b_ch(&b, ' '); b_s(&b, g_rt_vn[0] ? g_rt_vn : "-"); b_ch(&b, ' '); b_int(&b, g_rt_idx); b_ch(&b, '\n');
    if (g_rt_hasS) { b_s(&b, "S "); b_fix(&b, g_rt_S[0], 3); b_ch(&b, ' '); b_fix(&b, g_rt_S[1], 3); b_ch(&b, ' '); b_fix(&b, g_rt_S[2], 3); b_ch(&b, '\n'); }
    if (g_rt_hasE) { b_s(&b, "E "); b_fix(&b, g_rt_E[0], 3); b_ch(&b, ' '); b_fix(&b, g_rt_E[1], 3); b_ch(&b, ' '); b_fix(&b, g_rt_E[2], 3); b_ch(&b, '\n'); }
    if (g_rt_camRec) b_s(&b, "K 1\n");
    for (i = 0; i < n; i++) {
        b_int(&b, (long)g_rt_s[i].t); b_ch(&b, ' '); b_int(&b, (long)g_rt_s[i].p); b_ch(&b, ' ');
        b_fix(&b, g_rt_s[i].x, 3); b_ch(&b, ' '); b_fix(&b, g_rt_s[i].y, 3);
        if (g_rt_camRec) { b_ch(&b, ' '); b_fix(&b, g_rt_s[i].c, 2); b_ch(&b, ' '); b_fix(&b, g_rt_s[i].v, 2); }
        b_ch(&b, '\n');
        if (b.n > sizeof buf - 100) {
            if (!flog_write(path, b.p, b.n, first)) return 0;
            first = 0; b_init(&b, buf, sizeof buf);
        }
    }
    if (b.n || first) if (!flog_write(path, b.p, b.n, first)) return 0;
    return 1;
}
static const char* rt_num(const char* s, double* out) {
    double v = 0, f = 1; int neg = 0, any = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { f /= 10; v += (*s - '0') * f; s++; any = 1; } }
    if (!any) return NULL;
    *out = neg ? -v : v;
    return s;
}
static const char* rt_word(const char* s, char* out, int cap) {
    int k = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && *s != '\n' && *s != '\r') { if (k < cap - 1) out[k++] = *s; s++; }
    out[k] = 0;
    return s;
}
static int rt_load(void) {
    char path[320]; int fd, cnt = 0; long tot = 0; const char* p; double v[4];
    if (!rt_path(path, sizeof path)) return 0;
    fd = open(path, 0);
    if (fd < 0) return 0;
    for (;;) { long r = read(fd, g_rt_fbuf + tot, RT_FBUF - 1 - tot); if (r <= 0) break; tot += r; if (tot >= RT_FBUF - 1) break; }
    close(fd);
    g_rt_fbuf[tot] = 0;
    g_rt_hasS = g_rt_hasE = 0; g_rt_hn[0] = g_rt_vn[0] = 0; g_rt_idx = -1; g_rt_camRec = 0;
    for (p = g_rt_fbuf; *p; ) {
        const char* q = p;
        if (*q == 'H') {
            q = rt_word(q + 1, g_rt_hn, sizeof g_rt_hn); q = rt_word(q, g_rt_vn, sizeof g_rt_vn);
            if (seq(g_rt_hn, "-")) g_rt_hn[0] = 0;
            if (seq(g_rt_vn, "-")) g_rt_vn[0] = 0;
            if ((q = rt_num(q, &v[0]))) g_rt_idx = (int)v[0];
        } else if (*q == 'S' || *q == 'E') {
            int isS = (*q == 'S'); float* dst = isS ? g_rt_S : g_rt_E;
            q++;
            if ((q = rt_num(q, &v[0])) && (q = rt_num(q, &v[1])) && (q = rt_num(q, &v[2]))) {
                dst[0] = (float)v[0]; dst[1] = (float)v[1]; dst[2] = (float)v[2];
                if (isS) g_rt_hasS = 1; else g_rt_hasE = 1;
            }
        } else if (*q == 'K') {
            g_rt_camRec = 1;
        } else if (*q >= '0' && *q <= '9') {
            if ((q = rt_num(q, &v[0])) && (q = rt_num(q, &v[1])) && (q = rt_num(q, &v[2])) && (q = rt_num(q, &v[3])) && cnt < RT_MAX) {
                double cc = 0, vv = 0; const char* q2 = rt_num(q, &cc);
                if (q2) rt_num(q2, &vv);
                g_rt_s[cnt].t = (unsigned)v[0]; g_rt_s[cnt].p = v[1] > 0.5 ? 1u : 0u; g_rt_s[cnt].x = (float)v[2]; g_rt_s[cnt].y = (float)v[3];
                g_rt_s[cnt].c = (float)cc; g_rt_s[cnt].v = (float)vv;
                cnt++;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    g_rt_n = cnt;
    return cnt;
}

/* ───────────── команда ───────────── */
/* слова: 1 rec/record, 2 stop/cancel, 4 play, 16 status, 32 probe, 64 force, 128 nocam */
static int rt_parse(void* arr) {
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
            if (seq(tok, "rec") || seq(tok, "record")) w |= 1;
            else if (seq(tok, "stop") || seq(tok, "cancel")) w |= 2;
            else if (seq(tok, "play")) w |= 4;
            else if (seq(tok, "status") || seq(tok, "diag")) w |= 16;
            else if (seq(tok, "probe")) w |= 32;
            else if (seq(tok, "force")) w |= 64;
            else if (seq(tok, "nocam")) w |= 128;
        }
    }
    return w;
}

static int rt_my_pos(float* me) {
    if (!g_local || !nc_obj_ok(g_local)) return 0;
    return read_pos(g_local, me);
}
static void b_pos(Buf* b, const float* p) { b_fix(b, p[0], 1); b_s(b, ", "); b_fix(b, p[1], 1); b_s(b, ", "); b_fix(b, p[2], 1); }
static double rt_dist2(const float* a, const float* c) { double dx = a[0] - c[0], dy = a[1] - c[1]; return __builtin_sqrt(dx * dx + dy * dy); }   /* по горизонтали, как d2 в /near */

static void rt_report(int code) {                                  /* главный поток */
    Buf b; char msg[360]; float me[3];
    b_init(&b, msg, sizeof msg);
    if (code == 1) {
        b_s(&b, "Маршрут пройден");
        if (g_rt_hasE && rt_my_pos(me)) { b_s(&b, ". До точки конца записи: "); b_fix(&b, rt_dist2(me, g_rt_E), 1); b_s(&b, " м"); }
    } else if (code == 2) b_s(&b, "Маршрут: повтор прерван — вы взялись за стик");
    else if (code == 3) { b_s(&b, "Маршрут: запись заполнена. "); rt_stop_rec(&b); }
    else if (code == 4) b_s(&b, "Маршрут: стик пропал (HUD пересоздан?), остановлено. Повторите /route probe");
    else if (code == 5) { b_s(&b, "Маршрут: лимит 10 минут. "); rt_stop_rec(&b); }
    else return;
    chat_say(msg);
}

static void rt_stop_rec(Buf* b) {                                  /* главный поток; режим записи уже выключен или выключается здесь */
    int n; float me[3];
    __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE);
    n = g_rt_n;
    if (!g_rt_started || n < 1) { b_s(b, "Запись остановлена: вы так и не брались за стик, ничего не записано"); return; }
    rt_push((unsigned)(rt_now() - g_rt_t0), 0, 0, 0, g_rt_lc, g_rt_lv);   /* отпускание стика — чтобы повтор знал длительность */
    n = g_rt_n;
    g_rt_idx = 0; g_rt_hn[0] = '-'; g_rt_hn[1] = 0; g_rt_vn[0] = '-'; g_rt_vn[1] = 0;
    g_rt_hasE = rt_my_pos(me); if (g_rt_hasE) { g_rt_E[0] = me[0]; g_rt_E[1] = me[1]; g_rt_E[2] = me[2]; }
    b_s(b, "Записано: "); b_int(b, n); b_s(b, " точек, "); b_fix(b, (double)g_rt_s[n - 1].t / 1000.0, 1); b_s(b, " с");
    if (g_rt_hasS && g_rt_hasE) { b_s(b, ", от старта до финиша по прямой "); b_fix(b, rt_dist2(g_rt_S, g_rt_E), 1); b_s(b, " м"); }
    b_s(b, rt_save() ? ". Сохранено в HLOG/route.txt. Повтор: /route play" : ". Файл сохранить не удалось (только в памяти). Повтор: /route play");
}

static void native_route_cmd(void* self, void* args, void* mi) {
    int w; Buf b; char msg[420]; float me[3];
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { rt_tick(); return; }   /* вызов из Post (тик), не из чата */
    nc_capture_ctx();                                                /* главный поток: берём контекст для тиков */
    w = rt_parse(args);
    dlog_i("нативная /route: вызвана, флаги ", w);
    b_init(&b, msg, sizeof msg);

    if (w & 32) {                                                    /* probe */
        int ok = rt_find();
        if (ok) {
            float x = 0, y = 0; int rd = rt_stick_read(&x, &y);
            b_s(&b, "Стик найден: "); b_s(&b, g_rt_key[0] ? g_rt_key : "-");
            b_s(&b, rd ? ", значение (" : ", значение НЕ читается ("); b_fix(&b, x, 2); b_s(&b, "; "); b_fix(&b, y, 2); b_ch(&b, ')');
        } else b_s(&b, "Стик НЕ найден (причина в near.log)");
        if (rt_cam_find()) { float c = 0, v = 0; rt_cam_read(&c, &v); b_s(&b, " | камера: поворот "); b_fix(&b, c, 1); b_s(&b, ", наклон "); b_fix(&b, v, 1); }
        else b_s(&b, " | камера: НЕ найдена (см. near.log)");
        b_s(&b, g_nc_ctx ? " | тики: да" : " | тики: НЕТ");
        dlog2("маршрут probe: ", msg);
        chat_say(msg);
        return;
    }
    if (w & 16) {                                                    /* status */
        b_s(&b, "Маршрут: "); b_s(&b, g_rt_mode == 1 ? "идёт запись" : g_rt_mode == 2 ? "идёт повтор" : "ничего не идёт");
        b_s(&b, ", точек в памяти "); b_int(&b, g_rt_n);
        if (g_rt_n > 0) { b_s(&b, " ("); b_fix(&b, (double)g_rt_s[g_rt_n - 1].t / 1000.0, 1); b_s(&b, " с)"); }
        b_s(&b, g_nc_ctx ? ", тики: да" : ", тики: нет");
        chat_say(msg);
        return;
    }
    if (w & 2) {                                                     /* stop */
        if (g_rt_mode == 1) { rt_stop_rec(&b); chat_say(msg); return; }
        if (g_rt_mode == 2) {
            rt_play_stop(0);
            b_s(&b, "Повтор остановлен"); chat_say(msg); return;
        }
        b_s(&b, "Маршрут: сейчас ничего не идёт"); chat_say(msg); return;
    }
    if (w & 1) {                                                     /* rec */
        if (g_rt_mode) { b_s(&b, "Маршрут: сначала /route stop"); chat_say(msg); return; }
        if (!g_nc_ctx) { b_s(&b, "Маршрут: нет главного потока для тиков (см. near.log)"); chat_say(msg); return; }
        if (rt_find() < 1) { b_s(&b, "Маршрут: стик не найден (HUD с экранным стиком не создан? см. near.log, /route probe)"); chat_say(msg); return; }
        g_rt_camRec = rt_cam_find(); g_rt_lc = g_rt_lv = 0;
        g_rt_n = 0; g_rt_started = 0; g_rt_hasS = rt_my_pos(me); g_rt_hasE = 0;
        if (g_rt_hasS) { g_rt_S[0] = me[0]; g_rt_S[1] = me[1]; g_rt_S[2] = me[2]; }
        g_rt_tickPending = 0;
        __atomic_store_n(&g_rt_mode, 1, __ATOMIC_RELEASE);
        b_s(&b, "Запись маршрута: возьмитесь за левый стик и идите. Конец: /route stop. ");
        b_s(&b, g_rt_camRec ? "Камера пишется вместе со стиком; прыжок и спринт не пишутся" : "Камера НЕ найдена: пишется только стик (при повторе камеру выставляйте сами); прыжок и спринт не пишутся");
        chat_say(msg);
        return;
    }
    if (w & 4) {                                                     /* play */
        int i;
        if (g_rt_mode) { b_s(&b, "Маршрут: сначала /route stop"); chat_say(msg); return; }
        if (g_rt_n < 2 && rt_load() < 2) { b_s(&b, "Маршрут: записи нет. Сначала /route rec"); chat_say(msg); return; }
        if (!g_nc_ctx) { b_s(&b, "Маршрут: нет главного потока для тиков (см. near.log)"); chat_say(msg); return; }
        if (rt_find() < 1) { b_s(&b, "Маршрут: стик не найден (HUD с экранным стиком не создан? см. near.log, /route probe)"); chat_say(msg); return; }
        if (g_rt_hasS && !(w & 64) && rt_my_pos(me)) {
            double d = rt_dist2(me, g_rt_S);
            if (d > 2.0) {
                b_s(&b, "Маршрут: вы в "); b_fix(&b, d, 1); b_s(&b, " м от точки начала записи (");  b_pos(&b, g_rt_S);
                b_s(&b, "). Встаньте туда и повторите, или /route play force");
                chat_say(msg); return;
            }
        }
        g_rt_camOn = 0;
        if (g_rt_camRec && !(w & 128)) g_rt_camOn = rt_cam_find();
        for (i = 0; i < 4; i++) { g_rt_wh[i][0] = 0; g_rt_wh[i][1] = 0; }
        g_rt_whi = 0; g_rt_down = 0; g_rt_tickPending = 0;
        g_rt_pi = 0; g_rt_wrote = 0; g_rt_bad = 0; g_rt_t0 = rt_now() + (g_rt_camOn ? 250 : 0);       /* с камерой — короткий предстарт: сначала выставляется она */
        __atomic_store_n(&g_rt_mode, 2, __ATOMIC_RELEASE);
        b_s(&b, "Повтор маршрута: "); b_fix(&b, (double)g_rt_s[g_rt_n - 1].t / 1000.0, 1); b_s(&b, " с. ");
        b_s(&b, g_rt_camOn ? "Камера выставляется сама. " : (g_rt_camRec ? (w & 128 ? "Камера — ваша (nocam). " : "Камеру выставить не удалось: смотрите туда же, куда при записи. ") : "В записи нет камеры: смотрите туда же, куда при записи. "));
        b_s(&b, "Остановить: /route stop (или возьмитесь за стик)");
        chat_say(msg);
        return;
    }
    chat_say("Маршрут: /route rec — запись, /route stop — стоп, /route play — повтор (/route play force — без проверки места), /route status, /route probe");
}

/* ───────────── самозапуск ───────────── */
static int rt_boot_once(void) {
    void *cw, *fd, *dict = NULL, *kls, *mset, *f_mp, *f_ii, *nd; int size, off_mp, off_ii;
    if (!g_tplDelegate || !g_img) { dlog("нативная /route: нет образца делегата чат-команды"); return 0; }
    ensure_attached();
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    fd = cw ? A.class_get_field_from_name(cw, "m_ChatCommandEvents") : NULL;
    if (!fd) { dlog("нативная /route: поле WebViewSystem.m_ChatCommandEvents не найдено"); return 0; }
    A.field_static_get_value(fd, &dict);
    if (!dict) { dlog("нативная /route: реестр чат-команд не найден"); return 0; }
    kls = A.object_get_class(g_tplDelegate);
    size = A.class_instance_size(kls);
    f_mp = find_field(kls, "method_ptr");
    if (size < 0x40 || size > 0x200 || !f_mp) { dlog("нативная /route: странный делегат чат-команды"); return 0; }
    off_mp = A.field_get_offset(f_mp);
    f_ii = find_field(kls, "invoke_impl");
    off_ii = f_ii ? A.field_get_offset(f_ii) : 0;
    mset = find_method(A.object_get_class(dict), "set_Item", 2);
    if (!mset) { dlog("нативная /route: у реестра нет set_Item"); return 0; }
    g_nc_kTpl = kls; g_nc_dsize = size; g_nc_offmp = off_mp; g_nc_offii = off_ii;                   /* нужно для nc_capture_ctx / Post */
    nd = clone_cmd_delegate(kls, size, off_mp, off_ii, (uptr)native_route_cmd);
    if (nd && (cmd_put(dict, mset, "/route", nd) & cmd_put(dict, mset, "route", nd))) { dlog("нативная /route: зарегистрирована («/route» и «route»)"); return 1; }
    dlog("нативная /route: НЕ зарегистрирована");
    return 0;
}
static void* rt_main(void* arg) {
    int w; unsigned long t = 0;
    (void)arg;
    for (w = 0; !__atomic_load_n(&g_started, __ATOMIC_ACQUIRE); w++) usleep(500000);
    if (off_switch()) return NULL;
    for (w = 0; !__atomic_load_n(&g_cmdDone, __ATOMIC_ACQUIRE); w++) usleep(500000);
    if (__atomic_load_n(&g_cmdDone, __ATOMIC_ACQUIRE) != 1) { dlog("нативная /route: Near не зарегистрировал команды, /route не будет"); return NULL; }
    for (w = 0; w < 120 && !g_nc_kTpl; w++) usleep(500000);          /* пусть /nocol закончит свою запись в реестр: словарь не потокобезопасен */
    usleep(2000000);
    dlog("нативная /route: Near готов, регистрирую");
    if (!rt_boot_once()) return NULL;
    pthread_create(&t, NULL, rt_worker, NULL);
    return NULL;
}
#ifndef NEAR_TEST
__attribute__((constructor)) static void route_ctor(void) {
    unsigned long t = 0;
    pthread_create(&t, NULL, rt_main, NULL);
}
#endif
