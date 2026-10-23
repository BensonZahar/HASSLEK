/*
 * hassle_route.c — необязательный модуль «/route»: записать свой маршрут пешком и повторить его ТОЧНО ТАК ЖЕ.
 *
 * hassle_near.c и hassle_nocol.c НЕ МЕНЯЮТСЯ. Этот файл включает hassle_nocol.c (а тот — hassle_near.c), сам запускает свой поток
 * и сам регистрирует команду. build_near.py собирает hassle_route.c, если он лежит рядом.
 * КАК УБРАТЬ: удалите hassle_route.c и пересоберите .so — всё как раньше.
 *
 * Команда в чате (нативная, как /rec и /nocol):
 *   /route rec            начать запись; запись стартует, когда вы возьмётесь за левый джойстик
 *   /route stop           остановить запись (или остановить повтор); запись сохраняется в HLOG/route.txt
 *   /route play           повторить записанный маршрут (без записи в памяти — берёт HLOG/route.txt)
 *   /route play force     то же, но без проверки «вы стоите там же, где начинали запись»
 *   /route play nocam     повтор без выставления камеры (камеру держите сами)
 *   /route status         состояние
 *   /route probe          найти джойстики на экране и показать их значения (диагностика)
 *
 * Как это работает (проверено по дампу игры, VirtualAxis.GetAxis):
 *   • Левый стик — Hassle.Utilities.Input.VirtualControls.VirtualJoystick (наследник VirtualAxis). Игра читает его так:
 *     m_Pressed (bool, +0x28) == 0 → ось 0; иначе m_Delta.x (+0x2C) для горизонтальной оси и m_Delta.y (+0x30) для вертикальной.
 *     Нажатие пальцем пишет в эти же поля (OnTouchStart/OnTouchMove/OnTouchEnd).
 *   • Запись: фоновый поток каждые ~5 мс читает m_Pressed и m_Delta выбранного джойстика и сохраняет значение с временем, когда оно изменилось.
 *   • Повтор: тот же поток каждые ~5 мс записывает в m_Pressed/m_Delta значение, которое было в этот момент записи. Игра видит обычный ввод.
 *   • Никаких вызовов IL2CPP из фонового потока: только чтение/запись полей по смещениям. Поиск джойстиков (FindObjectsOfType) —
 *     только в главном потоке, в обработчике чат-команды. Сообщения в чат по окончании идут через UnitySynchronizationContext.Post
 *     (механизм из /nocol).
 *   • Камера: Game.MainCamera.m_Aim (CinemachinePOV) → m_HorizontalAxis.Value (поворот, градусы) и m_VerticalAxis.Value (наклон).
 *     Запись пишет их вместе со стиком; повтор сначала выставляет камеру (пауза ~0,25 с), потом идёт, повторяя и повороты камеры. Так
 *     движение (оно считается относительно камеры) идёт в ту же сторону, как при записи, откуда бы вы ни смотрели перед /route play.
 *     Если камера не найдена, запись идёт только по стику (сообщит в чат), а при повторе камеру нужно выставить самому.
 *   • Если во время повтора вы возьмётесь за джойстик сами, повтор прерывается (мод замечает, что значение отличается от записанного).
 *
 * Ограничения первой версии (важно):
 *   • Повторяются левый стик и камера. Прыжок и спринт (кнопки) НЕ записываются. Положение игрока перед /route play проверяется по
 *     точке начала записи (не дальше 2 м).
 *   • Во время повтора камера занята модом: крутить её пальцем бесполезно (/route play nocam — чтобы камеру держали вы).
 *   • Точность: ввод повторяется с точностью до миллисекунд, но игра идёт по кадрам, так что на длинном маршруте возможен небольшой снос.
 *     По окончании мод пишет в чат, на сколько метров вы не дошли до точки конца записи.
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
typedef struct { unsigned t; float x, y; unsigned p; float c, v; } RtS;   /* t — мс от начала, p — m_Pressed, x/y — m_Delta, c/v — поворот/наклон камеры */
static RtS g_rt_s[RT_MAX];
static volatile int g_rt_n;
static volatile int g_rt_mode;       /* 0 — ничего, 1 — запись, 2 — повтор */
static volatile int g_rt_evt;        /* для сообщения в чат из главного потока: 1 повтор закончен, 2 вы взялись за джойстик, 3 запись переполнена, 4 джойстик пропал, 5 лимит времени */
static char g_rt_hn[64], g_rt_vn[64];
static int g_rt_idx = -1;
static int g_rt_hasS, g_rt_hasE;
static float g_rt_S[3], g_rt_E[3];

static void* g_rt_j[RT_MAXJ];
static void* g_rt_jk[RT_MAXJ];                                   /* заголовок объекта (указатель на класс) на момент поиска — для проверки перед записью */
static char g_rt_jh[RT_MAXJ][64], g_rt_jv[RT_MAXJ][64];
static int g_rt_nj;
static int g_rt_offP = 0x28, g_rt_offD = 0x2C;
static int g_rt_sel = -1;
static long long g_rt_t0;
static int g_rt_pi;
static int g_rt_wrote, g_rt_bad;
static unsigned g_rt_wp;
static float g_rt_wx, g_rt_wy;
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

/* ───────────── поиск джойстиков (ТОЛЬКО главный поток) ───────────── */
static void rt_name(void* o, const char* field, char* out, int cap) {
    void* s = ref_field_s(o, field); int n, i; const u16* ch;
    out[0] = 0;
    if (!s || !nc_readable(s, 24)) return;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 60 || !ch) return;
    utf16_to_utf8(ch, n, out, cap);
    for (i = 0; out[i]; i++) if (out[i] == ' ') out[i] = '_';
}

static int rt_find(void) {
    void *kt, *img, *ko, *m, *tobj, *arr, *exc = NULL, *args[1], *fP, *fD; long len, i; char et[240];
    g_rt_nj = 0;
    if (!g_img || !rt_api()) { dlog("маршрут: нет il2cpp_class_get_type / il2cpp_type_get_object"); return 0; }
    kt = A.class_from_name(g_img, "Hassle.Utilities.Input.VirtualControls", "VirtualJoystick");
    if (!kt) { dlog("маршрут: класс VirtualJoystick не найден"); return 0; }
    fP = find_field(kt, "m_Pressed"); fD = find_field(kt, "m_Delta");
    if (!fP || !fD) { dlog("маршрут: у VirtualJoystick нет полей m_Pressed / m_Delta"); return 0; }
    g_rt_offP = A.field_get_offset(fP); g_rt_offD = A.field_get_offset(fD);
    if (g_rt_offP < 0x10 || g_rt_offP > 0x100 || g_rt_offD < 0x10 || g_rt_offD > 0x100) { dlog_i("маршрут: странное смещение полей джойстика ", g_rt_offP); return 0; }
    img = find_image_pre("UnityEngine.CoreModule");
    ko = img ? A.class_from_name(img, "UnityEngine", "Object") : NULL;
    m = ko ? A.class_get_method_from_name(ko, "FindObjectsOfType", 1) : NULL;
    if (!m) { dlog("маршрут: нет UnityEngine.Object.FindObjectsOfType(Type)"); return 0; }
    tobj = RA.type_get_object(RA.class_get_type(kt));
    if (!tobj) { dlog("маршрут: не получен Type для VirtualJoystick"); return 0; }
    args[0] = tobj;
    arr = A.runtime_invoke(m, NULL, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("маршрут: FindObjectsOfType бросил: ", et); return 0; }
    if (!arr || !nc_readable(arr, 4 * sizeof(void*))) { dlog("маршрут: FindObjectsOfType вернул пусто"); return 0; }
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 64) { dlog_i("маршрут: странная длина массива ", len); return 0; }
    for (i = 0; i < len && g_rt_nj < RT_MAXJ; i++) {
        void* o = *(void**)((char*)arr + 4 * sizeof(void*) + (uptr)i * sizeof(void*));
        if (!nc_obj_ok(o) || !nc_readable(o, g_rt_offD + 12)) continue;
        g_rt_j[g_rt_nj] = o; g_rt_jk[g_rt_nj] = *(void**)o;
        rt_name(o, "m_HorizontalInputName", g_rt_jh[g_rt_nj], 64);
        rt_name(o, "m_VerticalInputName", g_rt_jv[g_rt_nj], 64);
        g_rt_nj++;
    }
    dlog_i("маршрут: найдено джойстиков ", g_rt_nj);
    return g_rt_nj;
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

/* ───────────── чтение / запись полей джойстика (любой поток, без IL2CPP) ───────────── */
static int rt_read(int i, unsigned* p, float* x, float* y) {
    char* o;
    if (i < 0 || i >= g_rt_nj) return 0;
    o = (char*)g_rt_j[i];
    if (!o || !nc_readable(o, g_rt_offD + 12) || *(void**)o != g_rt_jk[i]) return 0;
    *p = *(volatile u8*)(o + g_rt_offP) ? 1u : 0u;
    *x = *(volatile float*)(o + g_rt_offD);
    *y = *(volatile float*)(o + g_rt_offD + 4);
    return 1;
}
static int rt_write(int i, unsigned p, float x, float y) {
    char* o;
    if (i < 0 || i >= g_rt_nj) return 0;
    o = (char*)g_rt_j[i];
    if (!o || !nc_readable(o, g_rt_offD + 12) || *(void**)o != g_rt_jk[i]) return 0;    /* объект исчез/подменён — не пишем */
    *(volatile float*)(o + g_rt_offD) = x;
    *(volatile float*)(o + g_rt_offD + 4) = y;
    *(volatile u8*)(o + g_rt_offP) = p ? 1 : 0;
    return 1;
}

/* ───────────── сообщение в главный поток ───────────── */
static void native_route_cmd(void* self, void* args, void* mi);
static void rt_post(void) {
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    if (!g_nc_ctx || !g_nc_postM || !g_nc_kCb) { dlog("маршрут: нет главного потока для сообщения в чат (итог смотрите в /route status)"); return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_nc_kCb, g_nc_dsize, g_nc_offmp, g_nc_offii, (uptr)native_route_cmd);
    st = A.string_new("route-evt");
    if (!cb || !st) { dlog("маршрут: не создался делегат для Post"); return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_nc_postM, g_nc_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("маршрут: Post бросил: ", et); }
}
static void rt_event(int code) {
    g_rt_evt = code;
    dlog_i("маршрут: событие ", code);
    rt_post();
}

/* ───────────── рабочий поток ───────────── */
static void rt_push(unsigned t, unsigned p, float x, float y, float c, float v) {
    int n = g_rt_n;
    if (n >= RT_MAX) return;
    g_rt_s[n].t = t; g_rt_s[n].p = p; g_rt_s[n].x = x; g_rt_s[n].y = y; g_rt_s[n].c = c; g_rt_s[n].v = v;
    __atomic_store_n(&g_rt_n, n + 1, __ATOMIC_RELEASE);
}
static float rt_abs(float v) { return v < 0 ? -v : v; }

static void rt_rec_step(void) {
    unsigned p; float x, y, c = 0, v = 0; long long now; int i;
    if (g_rt_camRec && !rt_cam_read(&c, &v)) { c = g_rt_lc; v = g_rt_lv; }
    if (g_rt_sel < 0) {                                          /* ждём, когда вы возьмётесь за стик */
        for (i = 0; i < g_rt_nj; i++) {
            if (rt_read(i, &p, &x, &y) && p && (x != 0 || y != 0)) {
                g_rt_sel = i; g_rt_t0 = rt_now();
                g_rt_lp = p; g_rt_lx = x; g_rt_ly = y; g_rt_lc = c; g_rt_lv = v;
                g_rt_n = 0;
                rt_push(0, p, x, y, c, v);
                dlog_i("маршрут: запись началась, джойстик ", i);
                return;
            }
        }
        return;
    }
    if (!rt_read(g_rt_sel, &p, &x, &y)) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(4); return; }
    now = rt_now() - g_rt_t0;
    if (p != g_rt_lp || rt_abs(x - g_rt_lx) > 0.004f || rt_abs(y - g_rt_ly) > 0.004f || (g_rt_camRec && (rt_abs(c - g_rt_lc) > 0.15f || rt_abs(v - g_rt_lv) > 0.15f))) {
        rt_push((unsigned)now, p, x, y, c, v);
        g_rt_lp = p; g_rt_lx = x; g_rt_ly = y; g_rt_lc = c; g_rt_lv = v;
    }
    if (g_rt_n >= RT_MAX - 2) { rt_push((unsigned)now, 0, 0, 0, g_rt_lc, g_rt_lv); __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(3); return; }
    if (now > RT_MAXMS) { rt_push((unsigned)now, 0, 0, 0, g_rt_lc, g_rt_lv); __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(5); return; }
}

static void rt_play_step(void) {
    long long now = rt_now() - g_rt_t0; int n = g_rt_n; unsigned cp; float cx, cy; RtS* s;
    if (n < 2) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); return; }
    if (now < 0) {                                               /* предстарт: сначала выставляем камеру в положение начала записи, стик ещё не трогаем */
        if (g_rt_camOn && !rt_cam_write(g_rt_s[0].c, g_rt_s[0].v)) { g_rt_camOn = 0; dlog("маршрут: камеру выставить не удалось, дальше только стик"); }
        return;
    }
    if (g_rt_wrote) {                                            /* кто-то (вы) изменил стик не так, как мы писали — уступаем */
        if (!rt_read(g_rt_sel, &cp, &cx, &cy)) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(4); return; }
        if (cp != g_rt_wp || rt_abs(cx - g_rt_wx) > 0.02f || rt_abs(cy - g_rt_wy) > 0.02f) {
            /* значение не наше: НЕ перезаписываем (иначе будем бороться с пальцем и никогда не заметим касание); 3 шага подряд (~15 мс) — уступаем */
            if (++g_rt_bad >= 3) {
                char t[200]; Buf b; b_init(&b, t, sizeof t);
                b_s(&b, "маршрут: повтор прерван, ждали p/x/y = "); b_int(&b, g_rt_wp); b_ch(&b, '/'); b_fix(&b, g_rt_wx, 3); b_ch(&b, '/'); b_fix(&b, g_rt_wy, 3);
                b_s(&b, ", прочитали "); b_int(&b, cp); b_ch(&b, '/'); b_fix(&b, cx, 3); b_ch(&b, '/'); b_fix(&b, cy, 3);
                dlog(t);
                __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(2);
            }
            return;
        }
        g_rt_bad = 0;
    }
    while (g_rt_pi + 1 < n && (long long)g_rt_s[g_rt_pi + 1].t <= now) g_rt_pi++;
    s = &g_rt_s[g_rt_pi];
    if (g_rt_pi >= n - 1 && now >= (long long)g_rt_s[n - 1].t) {            /* дошли до конца записи: отпускаем стик */
        rt_write(g_rt_sel, 0, 0, 0);
        __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE);
        rt_event(1);
        return;
    }
    if (g_rt_camOn && !rt_cam_write(s->c, s->v)) { g_rt_camOn = 0; dlog("маршрут: камера пропала (режим камеры сменился?), дальше только стик"); }
    if (!rt_write(g_rt_sel, s->p, s->x, s->y)) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(4); return; }
    g_rt_wrote = 1; g_rt_wp = s->p; g_rt_wx = s->x; g_rt_wy = s->y;
}

static void* rt_worker(void* arg) {
    (void)arg;
    for (;;) {
        int mode = __atomic_load_n(&g_rt_mode, __ATOMIC_ACQUIRE);
        if (mode == 0) { usleep(40000); continue; }
        if (mode == 1) rt_rec_step(); else rt_play_step();
        usleep(5000);
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

static void rt_pick_joystick(void) {                                 /* какой из найденных — тот, что в записи */
    int i, best = -1;
    for (i = 0; i < g_rt_nj; i++) if (seq(g_rt_jh[i], g_rt_hn) && seq(g_rt_jv[i], g_rt_vn)) { if (best < 0 || i == g_rt_idx) best = i; }
    if (best < 0 && g_rt_idx >= 0 && g_rt_idx < g_rt_nj) best = g_rt_idx;
    g_rt_sel = best;
}

static void rt_handle_event(void) {                                  /* главный поток, из Post */
    Buf b; char msg[360]; int e = g_rt_evt; float me[3];
    g_rt_evt = 0;
    b_init(&b, msg, sizeof msg);
    if (e == 1) {
        b_s(&b, "Маршрут пройден");
        if (g_rt_hasE && rt_my_pos(me)) { b_s(&b, ". До точки конца записи: "); b_fix(&b, rt_dist2(me, g_rt_E), 1); b_s(&b, " м"); }
    } else if (e == 2) b_s(&b, "Маршрут: повтор прерван — вы взялись за джойстик");
    else if (e == 3) b_s(&b, "Маршрут: запись заполнена, остановлена");
    else if (e == 4) b_s(&b, "Маршрут: джойстик пропал (экран сменился?), остановлено. Повторите /route probe");
    else if (e == 5) b_s(&b, "Маршрут: достигнут лимит 10 минут, остановлено");
    else return;
    chat_say(msg);
}

static void rt_stop_rec(Buf* b) {
    int n; float me[3];
    __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE);
    usleep(40000);                                                   /* дать рабочему потоку закончить шаг */
    n = g_rt_n;
    if (g_rt_sel < 0 || n < 1) { b_s(b, "Запись остановлена: вы так и не брались за джойстик, ничего не записано"); return; }
    rt_push((unsigned)(rt_now() - g_rt_t0), 0, 0, 0, g_rt_lc, g_rt_lv);   /* отпускание стика — чтобы повтор знал длительность */
    n = g_rt_n;
    g_rt_idx = g_rt_sel;
    { int i; for (i = 0; i < 64; i++) { g_rt_hn[i] = g_rt_jh[g_rt_sel][i]; g_rt_vn[i] = g_rt_jv[g_rt_sel][i]; } }
    g_rt_hasE = rt_my_pos(me); if (g_rt_hasE) { g_rt_E[0] = me[0]; g_rt_E[1] = me[1]; g_rt_E[2] = me[2]; }
    b_s(b, "Записано: "); b_int(b, n); b_s(b, " точек, "); b_fix(b, (double)g_rt_s[n - 1].t / 1000.0, 1); b_s(b, " с");
    if (g_rt_hasS && g_rt_hasE) { b_s(b, ", от старта до финиша по прямой "); b_fix(b, rt_dist2(g_rt_S, g_rt_E), 1); b_s(b, " м"); }
    b_s(b, rt_save() ? ". Сохранено в HLOG/route.txt. Повтор: /route play" : ". Файл сохранить не удалось (только в памяти). Повтор: /route play");
}

static void native_route_cmd(void* self, void* args, void* mi) {
    int w; Buf b; char msg[420]; float me[3];
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { rt_handle_event(); return; }   /* вызов из Post, не из чата */
    nc_capture_ctx();                                                /* главный поток: берём контекст для сообщений по окончании */
    w = rt_parse(args);
    dlog_i("нативная /route: вызвана, флаги ", w);
    b_init(&b, msg, sizeof msg);

    if (w & 32) {                                                    /* probe */
        int i, nj = rt_find();
        b_s(&b, "Джойстиков: "); b_int(&b, nj);
        for (i = 0; i < nj; i++) {
            unsigned p = 0; float x = 0, y = 0; rt_read(i, &p, &x, &y);
            b_s(&b, " | #"); b_int(&b, i); b_ch(&b, ' '); b_s(&b, g_rt_jh[i][0] ? g_rt_jh[i] : "-"); b_ch(&b, '/'); b_s(&b, g_rt_jv[i][0] ? g_rt_jv[i] : "-");
            b_s(&b, " нажат "); b_int(&b, p); b_s(&b, " ("); b_fix(&b, x, 2); b_s(&b, "; "); b_fix(&b, y, 2); b_ch(&b, ')');
        }
        b_s(&b, " | смещения m_Pressed/m_Delta: "); b_int(&b, g_rt_offP); b_ch(&b, '/'); b_int(&b, g_rt_offD);
        if (rt_cam_find()) { float c = 0, v = 0; rt_cam_read(&c, &v); b_s(&b, " | камера: поворот "); b_fix(&b, c, 1); b_s(&b, ", наклон "); b_fix(&b, v, 1); }
        else b_s(&b, " | камера: НЕ найдена (см. near.log)");
        dlog2("маршрут probe: ", msg);
        chat_say(msg);
        return;
    }
    if (w & 16) {                                                    /* status */
        b_s(&b, "Маршрут: "); b_s(&b, g_rt_mode == 1 ? "идёт запись" : g_rt_mode == 2 ? "идёт повтор" : "ничего не идёт");
        b_s(&b, ", точек в памяти "); b_int(&b, g_rt_n);
        if (g_rt_n > 0) { b_s(&b, " ("); b_fix(&b, (double)g_rt_s[g_rt_n - 1].t / 1000.0, 1); b_s(&b, " с)"); }
        b_s(&b, g_nc_ctx ? ", сообщения по окончании: да" : ", сообщения по окончании: нет (только near.log)");
        chat_say(msg);
        return;
    }
    if (w & 2) {                                                     /* stop */
        if (g_rt_mode == 1) { rt_stop_rec(&b); chat_say(msg); return; }
        if (g_rt_mode == 2) {
            __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); usleep(40000);
            rt_write(g_rt_sel, 0, 0, 0);
            b_s(&b, "Повтор остановлен"); chat_say(msg); return;
        }
        b_s(&b, "Маршрут: сейчас ничего не идёт"); chat_say(msg); return;
    }
    if (w & 1) {                                                     /* rec */
        if (g_rt_mode) { b_s(&b, "Маршрут: сначала /route stop"); chat_say(msg); return; }
        if (rt_find() < 1) { b_s(&b, "Маршрут: джойстик на экране не найден (см. near.log). Попробуйте /route probe"); chat_say(msg); return; }
        g_rt_camRec = rt_cam_find(); g_rt_lc = g_rt_lv = 0;
        g_rt_n = 0; g_rt_sel = -1; g_rt_hasS = rt_my_pos(me); g_rt_hasE = 0;
        if (g_rt_hasS) { g_rt_S[0] = me[0]; g_rt_S[1] = me[1]; g_rt_S[2] = me[2]; }
        __atomic_store_n(&g_rt_mode, 1, __ATOMIC_RELEASE);
        b_s(&b, "Запись маршрута: возьмитесь за левый джойстик и идите. Конец: /route stop. ");
        b_s(&b, g_rt_camRec ? "Камера пишется вместе со стиком; прыжок и спринт не пишутся" : "Камера НЕ найдена: пишется только стик (при повторе камеру выставляйте сами); прыжок и спринт не пишутся");
        chat_say(msg);
        return;
    }
    if (w & 4) {                                                     /* play */
        if (g_rt_mode) { b_s(&b, "Маршрут: сначала /route stop"); chat_say(msg); return; }
        if (g_rt_n < 2 && rt_load() < 2) { b_s(&b, "Маршрут: записи нет. Сначала /route rec"); chat_say(msg); return; }
        if (rt_find() < 1) { b_s(&b, "Маршрут: джойстик на экране не найден. Попробуйте /route probe"); chat_say(msg); return; }
        rt_pick_joystick();
        if (g_rt_sel < 0) { b_s(&b, "Маршрут: джойстик из записи не найден среди экранных (/route probe)"); chat_say(msg); return; }
        if (g_rt_hasS && !(w & 64) && rt_my_pos(me)) {
            double d = rt_dist2(me, g_rt_S);
            if (d > 2.0) {
                b_s(&b, "Маршрут: вы в "); b_fix(&b, d, 1); b_s(&b, " м от точки начала записи ("); b_pos(&b, g_rt_S);
                b_s(&b, "). Встаньте туда и повторите, или /route play force");
                chat_say(msg); return;
            }
        }
        g_rt_camOn = 0;
        if (g_rt_camRec && !(w & 128)) g_rt_camOn = rt_cam_find();
        g_rt_pi = 0; g_rt_wrote = 0; g_rt_bad = 0; g_rt_t0 = rt_now() + (g_rt_camOn ? 250 : 0);       /* с камерой — короткий предстарт: сначала выставляется она */
        __atomic_store_n(&g_rt_mode, 2, __ATOMIC_RELEASE);
        b_s(&b, "Повтор маршрута: "); b_fix(&b, (double)g_rt_s[g_rt_n - 1].t / 1000.0, 1); b_s(&b, " с. ");
        b_s(&b, g_rt_camOn ? "Камера выставляется сама. " : (g_rt_camRec ? (w & 128 ? "Камера — ваша (nocam). " : "Камеру выставить не удалось: смотрите туда же, куда при записи. ") : "В записи нет камеры: смотрите туда же, куда при записи. "));
        b_s(&b, "Остановить: /route stop (или возьмитесь за джойстик)");
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
