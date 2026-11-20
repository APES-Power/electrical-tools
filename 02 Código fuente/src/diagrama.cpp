#include "diagrama.hpp"

#include <adwaita.h>
#include <cairo-svg.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>

#include <filesystem>
#include <map>
#include <fstream>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {

constexpr double W = 190, H = 72;          // tamaño de bloque (unidades de mundo)
constexpr double GAP_X = 40, GAP_Y = 90;   // separación en la disposición automática
constexpr double GRID = 10;                // rejilla de ajuste
constexpr double H_DIST = 26, H_R = 10;    // flechas azules de conexión: distancia al bloque y radio
constexpr double ZOOM_MIN = 0.2, ZOOM_MAX = 3.0;
constexpr double W_MIN = 100, H_MIN = 40;   // tamaño mínimo al redimensionar
const char* const TIPO_GE = "G. ELEC.";
const char* const TIPO_SAI = "UPS";

struct Color { double r, g, b, a = 1; };
void fuente(cairo_t* cr, const Color& c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }
Color hex(unsigned v, double a = 1) { return {((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, (v & 0xFF) / 255.0, a}; }

const Color AZUL_SEL = hex(0x00a8ff);

// Colores de bloque (paleta de draw.io): relleno, borde y color de sus flechas
struct Estilo { Color relleno, borde, flecha; };
const Estilo E_ORIGEN{hex(0xfff2cc), hex(0xd6b656), hex(0x333333)};
const Estilo E_CUADRO{hex(0xdae8fc), hex(0x6c8ebf), hex(0x333333)};
const Estilo E_TRAFO{hex(0xf8cecc), hex(0xb85450), hex(0xb85450)};
const Estilo E_GRUPO{hex(0xd5e8d4), hex(0x82b366), hex(0x4d8a2e)};
const Estilo E_FV{hex(0xffe6cc), hex(0xd79b00), hex(0xc77c00)};
const Estilo E_SAI{hex(0xe1d5e7), hex(0x9673a6), hex(0x7a4f9a)};
const Estilo E_BAT{hex(0xc4dcff), hex(0x1565c0), hex(0x1565c0)};   // batería de condensadores: azul

bool oscuro() { return adw_style_manager_get_dark(adw_style_manager_get_default()); }

std::string fmt(double v, int d = 1) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(d) << v;
    std::string r = s.str();
    for (char& c: r) if (c == '.') c = ',';
    return r;
}

double ajustarRejilla(double v) { return std::round(v / GRID) * GRID; }

void rectRedondeado(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

double distSegmento(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l2, 0.0, 1.0) : 0;
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

RutaCuadro padreDe(const RutaCuadro& r) { return RutaCuadro(r.begin(), r.end() - (r.empty() ? 0 : 1)); }
bool empiezaPor(const RutaCuadro& r, const RutaCuadro& pref) {
    return r.size() >= pref.size() && std::equal(pref.begin(), pref.end(), r.begin());
}

void texto(cairo_t* cr, const std::string& markup, double x, double y, double ancho, const Color& c) {
    PangoLayout* l = pango_cairo_create_layout(cr);
    pango_layout_set_markup(l, markup.c_str(), -1);
    pango_layout_set_width(l, int(ancho * PANGO_SCALE));
    pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    fuente(cr, c);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

std::string esc(const std::string& s) {
    char* e = g_markup_escape_text(s.c_str(), -1);
    std::string r = e;
    g_free(e);
    return r;
}

GtkWidget* botonIcono(const char* icono, const char* tooltip) {
    GtkWidget* b = gtk_button_new_from_icon_name(icono);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_add_css_class(b, "flat");
    return b;
}

GtkWidget* botonTexto(const char* icono, const char* texto, const char* tooltip) {
    GtkWidget* b = gtk_button_new();
    GtkWidget* c = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(c), icono);
    adw_button_content_set_label(ADW_BUTTON_CONTENT(c), texto);
    gtk_button_set_child(GTK_BUTTON(b), c);
    if (tooltip) gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_add_css_class(b, "flat");
    return b;
}

const char* nombreTipo(TipoNodo t) {
    switch (t) {
        case TipoNodo::Trafo: return "transformador";
        case TipoNodo::Grupo: return "grupo electrógeno";
        case TipoNodo::SAI: return "SAI";
        case TipoNodo::Bateria: return "batería de condensadores";
        case TipoNodo::FV: return "instalación fotovoltaica";
        default: return "cuadro";
    }
}

// Lado por el que sale / entra un recorrido (0 arriba, 1 derecha, 2 abajo, 3 izquierda)
int ladoSalida(const std::vector<std::pair<double, double>>& pts) {
    for (size_t k = 1; k < pts.size(); k++) {
        double dx = pts[k].first - pts[0].first, dy = pts[k].second - pts[0].second;
        if (std::abs(dx) + std::abs(dy) < 1e-6) continue;
        return std::abs(dy) >= std::abs(dx) ? (dy > 0 ? 2 : 0) : (dx > 0 ? 1 : 3);
    }
    return 2;
}
int ladoEntrada(const std::vector<std::pair<double, double>>& pts) {
    const auto& q = pts.back();
    for (size_t k = pts.size() - 1; k-- > 0;) {
        double dx = q.first - pts[k].first, dy = q.second - pts[k].second;   // sentido de llegada
        if (std::abs(dx) + std::abs(dy) < 1e-6) continue;
        return std::abs(dy) >= std::abs(dx) ? (dy > 0 ? 0 : 2) : (dx > 0 ? 3 : 1);
    }
    return 0;
}
bool vertical(int lado) { return lado == 0 || lado == 2; }

// Punto de agarre del tramo intermedio de una flecha
std::pair<double, double> puntoCodo(const std::vector<std::pair<double, double>>& pts) {
    if (pts.size() >= 4) return {(pts[1].first + pts[2].first) / 2, (pts[1].second + pts[2].second) / 2};
    if (pts.size() == 3) return pts[1];
    return {(pts.front().first + pts.back().first) / 2, (pts.front().second + pts.back().second) / 2};
}

const char* prefijoTipo(TipoNodo t) {
    switch (t) {
        case TipoNodo::Trafo: return "T:";
        case TipoNodo::Grupo: return "G:";
        case TipoNodo::SAI: return "S:";
        case TipoNodo::Bateria: return "B:";
        case TipoNodo::FV: return "F:";
        default: return "C:";
    }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Construcción
// ─────────────────────────────────────────────────────────────────────────────

DiagramaCuadros::DiagramaCuadros(Proyecto* proyecto, CallbacksDiagrama callbacks)
    : p_(proyecto), cb_(std::move(callbacks)) {
    raiz_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget* barra = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(barra, 8); gtk_widget_set_margin_end(barra, 8);
    gtk_widget_set_margin_top(barra, 6); gtk_widget_set_margin_bottom(barra, 6);

    GtkWidget* bNuevo = botonTexto("list-add-symbolic", "Cuadro general", "Nuevo cuadro general de BT (origen)");
    g_signal_connect_swapped(bNuevo, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) { d->nuevoCuadroGeneral(); }), this);

    // Menú «Fuente»: transformador, grupo electrógeno, fotovoltaica
    GtkWidget* menu = gtk_menu_button_new();
    GtkWidget* mc = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(mc), "thunderbolt-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(mc), "Equipo");
    gtk_menu_button_set_child(GTK_MENU_BUTTON(menu), mc);
    gtk_widget_set_tooltip_text(menu, "Añadir un transformador, grupo electrógeno o instalación fotovoltaica");
    gtk_widget_add_css_class(menu, "flat");
    popFuentes_ = gtk_popover_new();
    GtkWidget* caja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    struct Opcion { const char* icono; const char* texto; TipoNodo tipo; };
    for (const Opcion& o: {Opcion{"thunderbolt-symbolic", "Transformador…", TipoNodo::Trafo},
                           Opcion{"network-cellular-gprs-symbolic", "Grupo electrógeno…", TipoNodo::Grupo},
                           Opcion{"uninterruptible-power-supply-symbolic", "SAI…", TipoNodo::SAI},
                           Opcion{"charge-symbolic", "Batería de condensadores…", TipoNodo::Bateria},
                           Opcion{"weather-clear-symbolic", "Fotovoltaica…", TipoNodo::FV}}) {
        GtkWidget* b = botonTexto(o.icono, o.texto, nullptr);
        gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)), GTK_ALIGN_START);
        g_object_set_data(G_OBJECT(b), "tipo", GINT_TO_POINTER(int(o.tipo)));
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
            auto* d = static_cast<DiagramaCuadros*>(data);
            gtk_popover_popdown(GTK_POPOVER(d->popFuentes_));
            if (d->cb_.nuevaFuente) d->cb_.nuevaFuente(TipoNodo(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "tipo"))));
        }), this);
        gtk_box_append(GTK_BOX(caja), b);
    }
    gtk_popover_set_child(GTK_POPOVER(popFuentes_), caja);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popFuentes_);

    btnSub_ = botonTexto("go-bottom-symbolic", "Subcuadro", "Añadir un subcuadro alimentado desde el cuadro seleccionado");
    btnEditar_ = botonTexto("document-edit-symbolic", "Editar…", "Editar el bloque seleccionado (doble clic)");
    btnEliminar_ = botonTexto("user-trash-symbolic", "Eliminar", "Eliminar el bloque o desconectar la flecha seleccionada (Supr)");
    GtkWidget* bOrg = botonTexto("view-grid-symbolic", "Organizar", "Colocar automáticamente los bloques");
    btnTrazado_ = botonTexto("edit-undo-symbolic", "Trazado automático", "Devolver la flecha seleccionada a su trazado automático");
    g_signal_connect_swapped(btnTrazado_, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) {
        d->reconstruir();
        int ia = d->indiceArista();
        if (ia < 0) return;
        d->p_->estilosAristas.erase(d->claveEstilo(d->aristas_[size_t(ia)]));
        if (d->cb_.cambioPosicion) d->cb_.cambioPosicion();
        d->actualizarBotones();
        gtk_widget_queue_draw(d->area_);
    }), this);
    g_signal_connect_swapped(btnSub_, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) { d->anadirSubcuadro(); }), this);
    g_signal_connect_swapped(btnEditar_, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) { d->editarSeleccion(); }), this);
    g_signal_connect_swapped(btnEliminar_, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) { d->eliminarSeleccion(); }), this);
    g_signal_connect_swapped(bOrg, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) {
        d->reconstruir();
        d->organizar();
        d->ajustar();
        if (d->cb_.cambioPosicion) d->cb_.cambioPosicion();
        gtk_widget_queue_draw(d->area_);
    }), this);

    for (GtkWidget* b: {bNuevo, menu, btnSub_, btnEditar_, btnEliminar_, btnTrazado_}) gtk_box_append(GTK_BOX(barra), b);
    gtk_box_append(GTK_BOX(barra), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(barra), bOrg);

    GtkWidget* ayuda = gtk_label_new("Flechas azules: conectar o crear · Flecha seleccionada: arrastra sus extremos o su tramo central · Doble clic: editar · Supr: eliminar");
    gtk_widget_add_css_class(ayuda, "dim-label");
    gtk_widget_add_css_class(ayuda, "caption");
    gtk_label_set_ellipsize(GTK_LABEL(ayuda), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(ayuda, TRUE);
    gtk_box_append(GTK_BOX(barra), ayuda);

    GtkWidget* bMenos = botonIcono("zoom-out-symbolic", "Alejar (Ctrl + rueda)");
    zoomLabel_ = gtk_button_new_with_label("100 %");
    gtk_widget_add_css_class(zoomLabel_, "flat");
    gtk_widget_set_tooltip_text(zoomLabel_, "Restablecer zoom al 100 %");
    GtkWidget* bMas = botonIcono("zoom-in-symbolic", "Acercar (Ctrl + rueda)");
    GtkWidget* bAjustar = botonIcono("zoom-fit-best-symbolic", "Ajustar a la ventana");
    g_signal_connect_swapped(bMenos, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) {
        d->setZoom(d->zoom_ / 1.2, gtk_widget_get_width(d->area_) / 2.0, gtk_widget_get_height(d->area_) / 2.0);
    }), this);
    g_signal_connect_swapped(bMas, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) {
        d->setZoom(d->zoom_ * 1.2, gtk_widget_get_width(d->area_) / 2.0, gtk_widget_get_height(d->area_) / 2.0);
    }), this);
    g_signal_connect_swapped(zoomLabel_, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) {
        d->setZoom(1.0, gtk_widget_get_width(d->area_) / 2.0, gtk_widget_get_height(d->area_) / 2.0);
    }), this);
    g_signal_connect_swapped(bAjustar, "clicked", G_CALLBACK(+[](DiagramaCuadros* d) {
        d->reconstruir(); d->ajustar(); gtk_widget_queue_draw(d->area_);
    }), this);
    for (GtkWidget* b: {bMenos, zoomLabel_, bMas, bAjustar}) gtk_box_append(GTK_BOX(barra), b);

    gtk_box_append(GTK_BOX(raiz_), barra);
    gtk_box_append(GTK_BOX(raiz_), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_widget_set_focusable(area_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), onDraw, this, nullptr);
    gtk_box_append(GTK_BOX(raiz_), area_);

    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(onDragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(onDragEnd), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(drag));

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(onMotion), this);
    g_signal_connect(motion, "leave", G_CALLBACK(onLeave), this);
    gtk_widget_add_controller(area_, motion);

    GtkEventController* scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(onScroll), this);
    gtk_widget_add_controller(area_, scroll);

    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(onKey), this);
    gtk_widget_add_controller(area_, key);

    g_signal_connect_swapped(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(gtk_widget_queue_draw), area_);

    actualizarBotones();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Modelo
// ─────────────────────────────────────────────────────────────────────────────

void DiagramaCuadros::reconstruir() {
    nodos_.clear();
    aristas_.clear();
    auto esSai = [&](const std::string& tag) {
        for (const EquipoEmergencia& e: p_->equipos) if (e.tag == tag && e.tipo == TIPO_SAI) return true;
        return false;
    };

    // Cuadros
    std::function<void(std::vector<Cuadro>&, const RutaCuadro&, int)> visita =
        [&](std::vector<Cuadro>& cs, const RutaCuadro& base, int padre) {
            for (size_t i = 0; i < cs.size(); i++) {
                RutaCuadro r = base; r.push_back(i);
                Nodo n;
                n.tipo = TipoNodo::Cuadro; n.ruta = r; n.c = &cs[i]; n.tag = cs[i].tag;
                n.x = &cs[i].diagX; n.y = &cs[i].diagY; n.w = &cs[i].diagW; n.h = &cs[i].diagH; n.padre = padre;
                nodos_.push_back(n);
                int yo = int(nodos_.size()) - 1;
                bool deSai = !cs[i].saiAlimentacion.empty() && esSai(cs[i].saiAlimentacion);   // su flecha sale del SAI
                if (padre >= 0 && !deSai) aristas_.push_back({padre, yo, TipoArista::Alimentacion});
                visita(cs[i].subcuadros, r, yo);
            }
        };
    visita(p_->cuadros, {}, -1);
    size_t nCuadros = nodos_.size();

    // Fuentes
    auto anadirFuente = [&](TipoNodo t, size_t idx, const std::string& tag, std::optional<double>& x, std::optional<double>& y,
                            std::optional<double>& w, std::optional<double>& h) {
        Nodo n;
        n.tipo = t; n.idx = idx; n.tag = tag; n.x = &x; n.y = &y; n.w = &w; n.h = &h;
        nodos_.push_back(n);
        return int(nodos_.size()) - 1;
    };
    for (size_t k = 0; k < p_->datos.transformadores.size(); k++) {
        Transformador& t = p_->datos.transformadores[k];
        int yo = anadirFuente(TipoNodo::Trafo, k, t.tag, t.diagX, t.diagY, t.diagW, t.diagH);
        for (size_t i = 0; i < nCuadros; i++)
            if (nodos_[i].padre < 0 && nodos_[i].c->trafo == t.tag) aristas_.push_back({yo, int(i), TipoArista::Trafo});
    }
    for (size_t k = 0; k < p_->equipos.size(); k++) {
        EquipoEmergencia& e = p_->equipos[k];
        bool ge = e.tipo == TIPO_GE, sai = e.tipo == TIPO_SAI;
        if (!ge && !sai) continue;
        int yo = anadirFuente(ge ? TipoNodo::Grupo : TipoNodo::SAI, k, e.tag, e.diagX, e.diagY, e.diagW, e.diagH);
        for (size_t i = 0; i < nCuadros; i++) {
            const Cuadro* c = nodos_[i].c;
            if (sai && c->saiAlimentacion == e.tag) { aristas_.push_back({yo, int(i), TipoArista::SalidaSAI}); continue; }
            bool usa = c->tagEmer && *c->tagEmer == e.tag;
            for (const Carga& x: c->cargas) if (x.tagEmer && *x.tagEmer == e.tag) usa = true;
            if (usa) aristas_.push_back({yo, int(i), ge ? TipoArista::Emergencia : TipoArista::SAI});
        }
        if (sai && !e.cuadroAlimentacion.empty())   // cuadro que alimenta al SAI
            for (size_t i = 0; i < nCuadros; i++)
                if (nodos_[i].c->tag == e.cuadroAlimentacion) { aristas_.push_back({int(i), yo, TipoArista::AlimSAI}); break; }
    }
    for (size_t k = 0; k < p_->datos.fotovoltaica.size(); k++) {
        InstalacionFV& f = p_->datos.fotovoltaica[k];
        int yo = anadirFuente(TipoNodo::FV, k, f.tag, f.diagX, f.diagY, f.diagW, f.diagH);
        for (size_t i = 0; i < nCuadros; i++)
            if (!f.cuadro.empty() && nodos_[i].c->tag == f.cuadro) { aristas_.push_back({yo, int(i), TipoArista::FV}); break; }
    }
    // Baterías de condensadores: equipo del CGBT (cuadro de origen)
    for (size_t i = 0; i < nCuadros; i++) {
        if (nodos_[i].padre >= 0 || nodos_[i].c->baterias.empty()) continue;
        Bateria& b = nodos_[i].c->baterias.front();
        int yo = anadirFuente(TipoNodo::Bateria, nodos_[i].ruta.front(), b.tag.empty() ? "CAP" : b.tag, b.diagX, b.diagY, b.diagW, b.diagH);
        aristas_.push_back({int(i), yo, TipoArista::AlimBat});
    }

    // Bloques sin posición (proyectos antiguos o bloques creados fuera del diagrama)
    bool ninguno = true;
    for (size_t i = 0; i < nodos_.size(); i++) if (tienePos(int(i))) ninguno = false;
    if (ninguno && !nodos_.empty()) { organizar(); return; }
    for (size_t i = 0; i < nCuadros; i++) {
        if (tienePos(int(i))) continue;
        double x, y;
        int dir;
        if (nodos_[i].padre >= 0 && tienePos(nodos_[i].padre)) {
            x = X(nodos_[i].padre); y = Y(nodos_[i].padre) + Hn(nodos_[i].padre) + GAP_Y; dir = 2;
        } else {
            x = 40; y = 40; dir = 1;
        }
        posicionLibre(x, y, dir, int(i));
        *nodos_[i].x = x; *nodos_[i].y = y;
    }
    colocarFuentesSinPosicion();
}

// Fuentes sin posición: encima del cuadro al que alimentan (o en la fila superior)
void DiagramaCuadros::colocarFuentesSinPosicion() {
    double yMin = 1e18, xMax = 40;
    for (size_t i = 0; i < nodos_.size(); i++)
        if (tienePos(int(i))) { yMin = std::min(yMin, Y(int(i))); xMax = std::max(xMax, X(int(i)) + Wn(int(i)) + GAP_X); }
    if (yMin > 1e17) yMin = 40 + H + GAP_Y;
    for (size_t i = 0; i < nodos_.size(); i++) {
        if (nodos_[i].tipo == TipoNodo::Cuadro || tienePos(int(i))) continue;
        int destino = -1, origen = -1;
        for (const Arista& a: aristas_) {
            if (a.desde == int(i) && destino < 0) destino = a.hasta;
            if (a.hasta == int(i) && (a.tipo == TipoArista::AlimSAI || a.tipo == TipoArista::AlimBat)) origen = a.desde;
        }
        double x, y;
        if (origen >= 0 && tienePos(origen)) {   // SAI alimentado desde un cuadro: a su derecha
            x = X(origen) + Wn(origen) + GAP_X + 20; y = Y(origen);
            posicionLibre(x, y, 1, int(i));
            *nodos_[i].x = x; *nodos_[i].y = y;
            continue;
        }
        if (destino >= 0 && tienePos(destino)) {
            x = X(destino);
            y = Y(destino) - H - GAP_Y;
            if (nodos_[i].tipo != TipoNodo::Trafo) x += Wn(destino) + GAP_X;   // trafo justo encima; el resto, en diagonal
        } else {
            x = xMax; y = yMin - H - GAP_Y;
        }
        posicionLibre(x, y, 0, int(i));
        *nodos_[i].x = x; *nodos_[i].y = y;
        xMax = std::max(xMax, x + Wn(int(i)) + GAP_X);
    }
}

double DiagramaCuadros::Wn(int i) const {
    const auto* w = nodos_[size_t(i)].w;
    return (w && *w && nodos_[size_t(i)].h->has_value()) ? **w : W;
}
double DiagramaCuadros::Hn(int i) const {
    const auto* h = nodos_[size_t(i)].h;
    return (h && *h && nodos_[size_t(i)].w->has_value()) ? **h : H;
}

DiagramaCuadros::Clave DiagramaCuadros::claveDe(int i) const {
    Clave k;
    k.tipo = nodos_[size_t(i)].tipo;
    if (k.tipo == TipoNodo::Cuadro) k.ruta = nodos_[size_t(i)].ruta; else k.tag = nodos_[size_t(i)].tag;
    return k;
}

int DiagramaCuadros::indice(const Clave& k) const {
    for (size_t i = 0; i < nodos_.size(); i++) if (claveDe(int(i)) == k) return int(i);
    return -1;
}

int DiagramaCuadros::indiceCuadro(const RutaCuadro& r) const {
    if (r.empty()) return -1;
    Clave k; k.ruta = r;
    return indice(k);
}

int DiagramaCuadros::indiceArista() const {
    if (sel_ != Sel::Arista) return -1;
    int a = indice(selA_), b = indice(selB_);
    for (size_t i = 0; i < aristas_.size(); i++)
        if (aristas_[i].desde == a && aristas_[i].hasta == b && aristas_[i].tipo == selTipoArista_) return int(i);
    return -1;
}

void DiagramaCuadros::posicionLibre(double& x, double& y, int dir, int ignorar) const {
    double w = ignorar >= 0 ? Wn(ignorar) : W, h = ignorar >= 0 ? Hn(ignorar) : H;
    auto choca = [&]() {
        for (size_t i = 0; i < nodos_.size(); i++) {
            if (int(i) == ignorar || !tienePos(int(i))) continue;
            if (x < X(int(i)) + Wn(int(i)) + 20 && X(int(i)) < x + w + 20 && y < Y(int(i)) + Hn(int(i)) + 20 && Y(int(i)) < y + h + 20) return true;
        }
        return false;
    };
    for (int n = 0; n < 200 && choca(); n++) {
        if (dir == 0 || dir == 2) x += w + GAP_X;   // crecer en horizontal
        else y += h + 30;                           // crecer en vertical
    }
    x = ajustarRejilla(x); y = ajustarRejilla(y);
}

RutaCuadro DiagramaCuadros::crearCuadro(const RutaCuadro& padre, double x, double y) {
    std::string tipo = padre.empty() ? "GENERAL BT" : "DIST. SEC.";
    Cuadro c;
    c.tipo = tipo;
    c.tag = p_->tagLibreCuadro(tipo);
    c.nombre = tipoCuadro(tipo).descripcion;
    c.diagX = ajustarRejilla(x);
    c.diagY = ajustarRejilla(y);
    RutaCuadro r;
    if (padre.empty()) {
        p_->cuadros.push_back(c);
        r = {p_->cuadros.size() - 1};
    } else {
        Cuadro* pc = p_->cuadroPorRuta(padre);
        if (!pc) return {};
        if (pc->configFases == "1F+N") c.configFases = "1F+N";
        pc->subcuadros.push_back(c);
        r = padre;
        r.push_back(pc->subcuadros.size() - 1);
    }
    Clave k; k.ruta = r;
    porMostrar_ = k;
    return r;
}

bool DiagramaCuadros::reconectar(const RutaCuadro& hijo, const RutaCuadro& nuevoPadreIn, RutaCuadro& nuevaRuta) {
    if (hijo.empty() || empiezaPor(nuevoPadreIn, hijo)) return false;   // no se puede colgar de sí mismo ni de un descendiente
    RutaCuadro padreActual = padreDe(hijo);
    if (padreActual == nuevoPadreIn) { nuevaRuta = hijo; return true; }
    Cuadro* c = p_->cuadroPorRuta(hijo);
    if (!c) return false;

    Cuadro copia = std::move(*c);
    std::vector<Cuadro>& origen = padreActual.empty() ? p_->cuadros : p_->cuadroPorRuta(padreActual)->subcuadros;
    origen.erase(origen.begin() + long(hijo.back()));

    // Al quitar el hijo, los hermanos posteriores (y sus descendientes) bajan un índice
    RutaCuadro nuevoPadre = nuevoPadreIn;
    size_t nivel = padreActual.size();
    if (nuevoPadre.size() > nivel && empiezaPor(nuevoPadre, padreActual) && nuevoPadre[nivel] > hijo.back()) nuevoPadre[nivel]--;

    std::vector<Cuadro>* destino = &p_->cuadros;
    if (!nuevoPadre.empty()) {
        Cuadro* np = p_->cuadroPorRuta(nuevoPadre);
        if (!np) return false;
        destino = &np->subcuadros;
        copia.trafo.clear();     // un CGBT que pasa a ser subcuadro deja de tener transformador propio
        copia.baterias.clear();  // ... y batería de condensadores (sólo en el CGBT)
    }
    destino->push_back(std::move(copia));
    nuevaRuta = nuevoPadre;
    nuevaRuta.push_back(destino->size() - 1);
    return true;
}

int DiagramaCuadros::indiceTag(TipoNodo t, const std::string& tag) const {
    for (size_t i = 0; i < nodos_.size(); i++) if (nodos_[i].tipo == t && nodos_[i].tag == tag) return int(i);
    return -1;
}

// ¿Puede el bloque «o» alimentar al bloque «d»?
bool DiagramaCuadros::valida(int o, int d, std::string* motivo) const {
    if (o < 0 || d < 0 || o == d) return false;
    const Nodo& no = nodos_[size_t(o)];
    const Nodo& nd = nodos_[size_t(d)];
    auto no_ = [&](const std::string& m) { if (motivo) *motivo = m; return false; };
    if (no.tipo == TipoNodo::Cuadro && nd.tipo == TipoNodo::Bateria) {   // la batería sólo cuelga de un CGBT
        if (no.padre >= 0) return no_("La batería de condensadores sólo puede conectarse a un CGBT (cuadro de origen)");
        if (!no.c->baterias.empty() && no.ruta.front() != nd.idx) return no_(no.tag + " ya tiene batería de condensadores");
        return true;
    }
    if (no.tipo == TipoNodo::Bateria) return no_("La batería de condensadores no alimenta a otros bloques");
    if (no.tipo == TipoNodo::Cuadro && nd.tipo == TipoNodo::SAI) {   // un cuadro puede alimentar a un SAI
        const EquipoEmergencia& u = p_->equipos[nd.idx];
        if (no.c->tagEmer && *no.c->tagEmer == u.tag)
            return no_("No se puede conectar: " + u.tag + " ya alimenta en emergencia a " + no.tag);
        return true;
    }
    if (nd.tipo != TipoNodo::Cuadro)
        return no_(no.tipo == TipoNodo::Cuadro ? "Un cuadro no puede alimentar a un " + std::string(nombreTipo(nd.tipo))
                                               : "Las fuentes sólo se conectan a cuadros");
    if (no.tipo == TipoNodo::Cuadro && empiezaPor(no.ruta, nd.ruta))
        return no_("No se puede conectar: " + nd.tag + " ya alimenta a " + no.tag);
    if (no.tipo == TipoNodo::SAI) {   // el cuadro va a colgar del SAI: no puede ser (ni contener) el que lo alimenta
        const std::string& f = p_->equipos[no.idx].cuadroAlimentacion;
        if (!f.empty()) {
            if (f == nd.tag) return no_(nd.tag + " alimenta al SAI " + no.tag + ": no puede colgar de él");
            std::function<bool(const Cuadro&)> contiene = [&](const Cuadro& c) {
                for (const Cuadro& h: c.subcuadros) if (h.tag == f || contiene(h)) return true;
                return false;
            };
            if (contiene(*nd.c)) return no_("No se puede: " + nd.tag + " contiene el cuadro que alimenta al SAI " + no.tag);
        }
    }
    if (no.tipo == TipoNodo::Trafo && nd.padre >= 0)
        return no_("Los transformadores sólo alimentan cuadros de origen (CGBT)");
    return true;
}

// Conexión arrastrando una flecha azul desde el bloque «o» hasta el bloque «d»
bool DiagramaCuadros::conectar(int o, int d) {
    auto aviso = [&](const std::string& m) { if (cb_.aviso) cb_.aviso(m); };
    std::string motivo;
    if (!valida(o, d, &motivo)) { if (!motivo.empty()) aviso(motivo); return false; }
    const Nodo no = nodos_[size_t(o)];
    const Nodo nd = nodos_[size_t(d)];
    Cuadro* c = nd.c;
    switch (no.tipo) {
        case TipoNodo::Cuadro: {
            if (nd.tipo == TipoNodo::Bateria) {   // mover la batería a otro CGBT
                Cuadro& viejo = p_->cuadros[nd.idx];
                if (&viejo == no.c) { aviso("La batería ya es de " + no.tag); return false; }
                no.c->baterias = viejo.baterias;
                no.c->compensacion = true;
                viejo.baterias.clear();
                viejo.compensacion = false;
                aviso("Batería de condensadores movida a " + no.tag);
                break;
            }
            if (nd.tipo == TipoNodo::SAI) {
                EquipoEmergencia& u = p_->equipos[nd.idx];
                if (u.cuadroAlimentacion == no.tag) { aviso(u.tag + " ya se alimenta de " + no.tag); return false; }
                u.cuadroAlimentacion = no.tag;
                aviso("El SAI " + u.tag + " se alimenta ahora de " + no.tag);
                break;
            }
            RutaCuadro nueva;
            if (padreDe(nd.ruta) == no.ruta) { aviso(c->tag + " ya está conectado a " + no.c->tag); return false; }
            nd.c->saiAlimentacion.clear();   // ahora lo alimenta un cuadro, no un SAI
            if (!reconectar(nd.ruta, no.ruta, nueva)) return false;
            sel_ = Sel::Nodo; selA_ = Clave{}; selA_.ruta = nueva;
            break;
        }
        case TipoNodo::Trafo:
            if (c->trafo == no.tag) { aviso(c->tag + " ya se alimenta de " + no.tag); return false; }
            c->trafo = no.tag;
            aviso(c->tag + " se alimenta ahora de " + no.tag);
            break;
        case TipoNodo::SAI:   // el cuadro cuelga del SAI (y queda asociado a él en emergencia)
            if (c->saiAlimentacion == no.tag) { aviso(c->tag + " ya cuelga del SAI " + no.tag); return false; }
            c->saiAlimentacion = no.tag;
            c->tagEmer = no.tag;
            aviso("El cuadro " + c->tag + " cuelga ahora del SAI " + no.tag);
            break;
        case TipoNodo::Grupo:
            if (c->tagEmer && *c->tagEmer == no.tag) { aviso(c->tag + " ya está asociado a " + no.tag); return false; }
            c->tagEmer = no.tag;
            aviso("Cuadro " + c->tag + " asociado " + (no.tipo == TipoNodo::SAI ? "al SAI " : "al grupo electrógeno ") + no.tag);
            break;
        case TipoNodo::Bateria:
            return false;
        case TipoNodo::FV:
            p_->datos.fotovoltaica[no.idx].cuadro = c->tag;
            aviso(no.tag + " conectada a " + c->tag);
            break;
    }
    if (cb_.cambioEstructura) cb_.cambioEstructura();
    notificarSeleccion();
    return true;
}

// Quita la conexión que representa la flecha (sin avisar si «silencioso»)
bool DiagramaCuadros::eliminarArista(int ia, bool silencioso) {
    if (ia < 0 || ia >= int(aristas_.size())) return false;
    auto aviso = [&](const std::string& m) { if (!silencioso && cb_.aviso) cb_.aviso(m); };
    const Arista a = aristas_[size_t(ia)];
    const Nodo o = nodos_[size_t(a.desde)];
    const Nodo dn = nodos_[size_t(a.hasta)];
    p_->estilosAristas.erase(claveEstilo(a));
    Cuadro* c = dn.c;   // nulo si el destino es un SAI
    std::string tag = dn.tag;
    switch (a.tipo) {
        case TipoArista::AlimBat:
            if (!silencioso && cb_.aviso) cb_.aviso("La batería debe estar conectada a un CGBT: para quitarla, elimina su bloque");
            return false;
        case TipoArista::SalidaSAI:
            c->saiAlimentacion.clear();
            if (c->tagEmer && *c->tagEmer == o.tag) c->tagEmer.reset();
            p_->moverCuadro(tag, "");   // deja de estar alimentado: pasa a ser un cuadro de origen
            sel_ = Sel::Nada;
            aviso(tag + " ya no cuelga del SAI " + o.tag + " y pasa a ser un origen");
            break;
        case TipoArista::AlimSAI:
            p_->equipos[dn.idx].cuadroAlimentacion.clear();
            sel_ = Sel::Nada;
            aviso("El SAI " + tag + " queda sin cuadro de alimentación");
            break;
        case TipoArista::Alimentacion: {
            RutaCuadro nueva;
            if (!reconectar(dn.ruta, {}, nueva)) return false;
            sel_ = Sel::Nodo; selA_ = Clave{}; selA_.ruta = nueva;
            aviso("Flecha eliminada: " + tag + " pasa a ser un origen");
            break;
        }
        case TipoArista::Trafo:
            c->trafo.clear();
            sel_ = Sel::Nada;
            aviso(tag + " queda sin transformador");
            break;
        case TipoArista::Emergencia:
        case TipoArista::SAI: {
            int n = 0;
            if (c->tagEmer && *c->tagEmer == o.tag) c->tagEmer.reset();
            for (Carga& x: c->cargas) if (x.tagEmer && *x.tagEmer == o.tag) { x.tagEmer.reset(); n++; }
            sel_ = Sel::Nada;
            if (cb_.aviso && (n || !silencioso))
                cb_.aviso("Cuadro " + tag + (n ? " y " + std::to_string(n) + " carga(s)" : "") + " desasociados de " + o.tag);
            break;
        }
        case TipoArista::FV:
            p_->datos.fotovoltaica[o.idx].cuadro.clear();
            sel_ = Sel::Nada;
            aviso(o.tag + " desconectada de " + tag);
            break;
    }
    return true;
}

// Arrastrar un extremo de una flecha hasta otro bloque: la conexión pasa a ese bloque
void DiagramaCuadros::reconectarArista(int ia, int extremo, int nuevo) {
    const Arista a = aristas_[size_t(ia)];
    int o = extremo == 0 ? nuevo : a.desde;
    int d = extremo == 1 ? nuevo : a.hasta;
    std::string motivo;
    if (!valida(o, d, &motivo)) { if (cb_.aviso && !motivo.empty()) cb_.aviso(motivo); return; }
    if (a.tipo == TipoArista::AlimBat) {   // la batería no se desconecta: se traslada a otro CGBT
        if (extremo != 0) { if (cb_.aviso) cb_.aviso("Arrastra el otro extremo hasta el CGBT al que quieres mover la batería"); return; }
        conectar(o, d);
        return;
    }
    TipoNodo to = nodos_[size_t(o)].tipo, td = nodos_[size_t(d)].tipo;
    std::string tagO = nodos_[size_t(o)].tag, tagD = nodos_[size_t(d)].tag;
    if (!eliminarArista(ia, true)) return;
    reconstruir();
    if (!conectar(indiceTag(to, tagO), indiceTag(td, tagD)) && cb_.cambioEstructura) cb_.cambioEstructura();
}

void DiagramaCuadros::organizar() {
    // Fila superior para las fuentes si las hay; debajo, el árbol: cuadro → subcuadros, SAI y batería;
    // SAI → cuadros que cuelgan de él
    bool hayFuentes = false;
    for (const Nodo& n: nodos_) if (n.tipo != TipoNodo::Cuadro) hayFuentes = true;
    int nivel0 = hayFuentes ? 1 : 0;

    size_t N = nodos_.size();
    std::vector<std::vector<int>> hijos(N);
    std::vector<bool> esHijo(N, false);
    for (const Arista& a: aristas_)
        if (a.tipo == TipoArista::Alimentacion || a.tipo == TipoArista::AlimSAI || a.tipo == TipoArista::AlimBat ||
            a.tipo == TipoArista::SalidaSAI) {
            hijos[size_t(a.desde)].push_back(a.hasta);
            esHijo[size_t(a.hasta)] = true;
        }
    for (Nodo& n: nodos_) if (n.tipo != TipoNodo::Cuadro) { n.x->reset(); n.y->reset(); }

    double hFila = H;   // altura de fila: la del bloque más alto
    for (size_t i = 0; i < N; i++) hFila = std::max(hFila, Hn(int(i)));
    std::vector<int> visitado(N, 0);
    std::function<double(int)> ancho = [&](int i) {
        double s = 0;
        for (int h: hijos[size_t(i)]) s += ancho(h);
        return std::max(Wn(i) + GAP_X, s);
    };
    std::function<void(int, double, int)> colocar = [&](int i, double x0, int nivel) {
        if (visitado[size_t(i)]++) return;
        double a = ancho(i);
        *nodos_[size_t(i)].x = ajustarRejilla(x0 + (a - Wn(i)) / 2);
        *nodos_[size_t(i)].y = ajustarRejilla(40 + nivel * (hFila + GAP_Y));
        double total = 0;
        for (int h: hijos[size_t(i)]) total += ancho(h);
        double x = x0 + (a - total) / 2;
        for (int h: hijos[size_t(i)]) { colocar(h, x, nivel + 1); x += ancho(h); }
    };
    double x = 40;
    for (size_t i = 0; i < N; i++) {
        bool raiz = !esHijo[i] && (nodos_[i].tipo == TipoNodo::Cuadro || (nodos_[i].tipo == TipoNodo::SAI && !hijos[i].empty()));
        if (!raiz) continue;
        colocar(int(i), x, nivel0);
        x += ancho(int(i)) + GAP_X;
    }

    // Resto de fuentes: en la fila superior, sobre los cuadros a los que alimentan (trafos primero)
    for (TipoNodo t: {TipoNodo::Trafo, TipoNodo::Grupo, TipoNodo::SAI, TipoNodo::FV, TipoNodo::Bateria}) {
        for (size_t i = 0; i < N; i++) {
            if (nodos_[i].tipo != t || tienePos(int(i))) continue;
            double sx = 0;
            int n = 0;
            for (const Arista& a: aristas_)
                if (a.desde == int(i) && tienePos(a.hasta)) { sx += X(a.hasta); n++; }
            double fx = n ? sx / n : x, fy = 40;
            posicionLibre(fx, fy, 0, int(i));
            *nodos_[i].x = fx; *nodos_[i].y = fy;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Geometría
// ─────────────────────────────────────────────────────────────────────────────

void DiagramaCuadros::aMundo(double sx, double sy, double& wx, double& wy) const {
    wx = (sx - ox_) / zoom_;
    wy = (sy - oy_) / zoom_;
}

int DiagramaCuadros::hitNodo(double wx, double wy) const {
    for (int i = int(nodos_.size()) - 1; i >= 0; i--)
        if (wx >= X(i) && wx <= X(i) + Wn(i) && wy >= Y(i) && wy <= Y(i) + Hn(i)) return i;
    return -1;
}

void DiagramaCuadros::handle(int n, int dir, double& x, double& y) const {
    double cx = X(n) + Wn(n) / 2, cy = Y(n) + Hn(n) / 2;
    switch (dir) {
        case 0: x = cx; y = Y(n) - H_DIST; break;
        case 1: x = X(n) + Wn(n) + H_DIST; y = cy; break;
        case 2: x = cx; y = Y(n) + Hn(n) + H_DIST; break;
        default: x = X(n) - H_DIST; y = cy; break;
    }
}

int DiagramaCuadros::hitHandle(int n, double wx, double wy) const {
    if (n < 0 || n >= int(nodos_.size())) return -1;
    for (int d = 0; d < 4; d++) {
        double hx, hy;
        handle(n, d, hx, hy);
        if (std::hypot(wx - hx, wy - hy) <= H_R + 4) return d;
    }
    return -1;
}

std::string DiagramaCuadros::claveEstiloTags(TipoNodo to, const std::string& tagO, TipoNodo td, const std::string& tagD) {
    return prefijoTipo(to) + tagO + ">" + prefijoTipo(td) + tagD;
}

std::string DiagramaCuadros::claveEstilo(const Arista& a) const {
    const Nodo& o = nodos_[size_t(a.desde)];
    const Nodo& d = nodos_[size_t(a.hasta)];
    return claveEstiloTags(o.tipo, o.tag, d.tipo, d.tag);
}

// Punto de conexión de un bloque bajo (wx, wy), como en draw.io: el centro (conexión automática,
// devuelve -1) si el ratón está en la zona central, o el centro del lado más próximo (0..3)
// Nodos de conexión de cada bloque (como en draw.io): tres por lado, al 25 %, 50 % y 75 %.
// Código de nodo: lado * 3 + k (k = 0, 1, 2); -1 = centro del bloque (conexión automática).
static const double FRAC_NODO[3] = {0.25, 0.5, 0.75};

int DiagramaCuadros::puntoConexion(int n, double wx, double wy, double& px, double& py) const {
    double cx = X(n) + Wn(n) / 2, cy = Y(n) + Hn(n) / 2;
    if (std::abs(wx - cx) <= Wn(n) * 0.2 && std::abs(wy - cy) <= Hn(n) * 0.2) { px = cx; py = cy; return -1; }
    int mejor = 0;
    double dmin = 1e18;
    for (int codigo = 0; codigo < 12; codigo++) {
        double x, y;
        puntoLado(n, codigo / 3, FRAC_NODO[codigo % 3], x, y);
        double dd = std::hypot(wx - x, wy - y);
        if (dd < dmin) { dmin = dd; mejor = codigo; px = x; py = y; }
    }
    return mejor;
}

// Bloque y punto de conexión bajo el ratón mientras se arrastra una flecha
void DiagramaCuadros::actualizarSnapConexion() {
    destinoSnap_ = -1; puntoSnap_ = -2;
    int n = hitNodo(curWX_, curWY_);
    if (n < 0) {   // cerca del borde también cuenta (margen de atracción)
        double m = 12 / zoom_;
        for (int i = int(nodos_.size()) - 1; i >= 0 && n < 0; i--)
            if (curWX_ >= X(i) - m && curWX_ <= X(i) + Wn(i) + m && curWY_ >= Y(i) - m && curWY_ <= Y(i) + Hn(i) + m) n = i;
    }
    if (n < 0) return;
    double px, py;
    destinoSnap_ = n;
    puntoSnap_ = puntoConexion(n, curWX_, curWY_, px, py);
}

// Fija el lado de salida (extremo 0) o de llegada (extremo 1) de la flecha que une dos bloques
void DiagramaCuadros::fijarLado(TipoNodo to, const std::string& tagO, TipoNodo td, const std::string& tagD, int extremo, int lado) {
    reconstruir();
    int o = indiceTag(to, tagO), d = indiceTag(td, tagD);
    for (const Arista& a: aristas_) {
        if (a.desde != o || a.hasta != d) continue;
        EstiloArista& e = p_->estilosAristas[claveEstilo(a)];
        int l = lado < 0 ? -1 : lado / 3;                     // «lado» es un código de nodo (-1 = centro)
        double f = lado < 0 ? 0.5 : FRAC_NODO[lado % 3];
        if (extremo == 0) { e.ladoOrigen = l; e.fracOrigen = f; } else { e.ladoDestino = l; e.fracDestino = f; }
        e.codo.reset();
        if (e.vacio()) p_->estilosAristas.erase(claveEstilo(a));
        return;
    }
}

const EstiloArista* DiagramaCuadros::estilo(const Arista& a) const {
    auto it = p_->estilosAristas.find(claveEstilo(a));
    return (it == p_->estilosAristas.end() || it->second.vacio()) ? nullptr : &it->second;
}

std::vector<std::pair<double, double>> DiagramaCuadros::recorrido(const Arista& a) const {
    const EstiloArista* e = estilo(a);
    return e ? recorridoEstilo(a, *e) : recorridoAuto(a);
}

// Nodo de conexión (código 0..11) bajo el ratón, o -1
int DiagramaCuadros::hitNodoConexion(int n, double wx, double wy) const {
    if (n < 0 || n >= int(nodos_.size())) return -1;
    double tol = 6 / zoom_;
    for (int codigo = 0; codigo < 12; codigo++) {
        double x, y;
        puntoLado(n, codigo / 3, FRAC_NODO[codigo % 3], x, y);
        if (std::hypot(wx - x, wy - y) <= tol) return codigo;
    }
    return -1;
}

// Punto del lado «lado» a una fracción «frac» de su longitud (0,5 = centro del lado)
void DiagramaCuadros::puntoLado(int n, int lado, double frac, double& x, double& y) const {
    switch (lado) {
        case 0: x = X(n) + Wn(n) * frac; y = Y(n); break;
        case 1: x = X(n) + Wn(n); y = Y(n) + Hn(n) * frac; break;
        case 2: x = X(n) + Wn(n) * frac; y = Y(n) + Hn(n); break;
        default: x = X(n); y = Y(n) + Hn(n) * frac; break;
    }
}

void DiagramaCuadros::puntoResize(int n, int k, double& x, double& y) const {
    const double fx[8] = {0, 0.5, 1, 1, 1, 0.5, 0, 0}, fy[8] = {0, 0, 0, 0.5, 1, 1, 1, 0.5};
    x = X(n) + fx[k] * Wn(n);
    y = Y(n) + fy[k] * Hn(n);
}

int DiagramaCuadros::hitResize(int n, double wx, double wy) const {
    if (n < 0 || n >= int(nodos_.size())) return -1;
    double tol = 6 / zoom_;
    for (int k = 0; k < 8; k++) {
        double x, y;
        puntoResize(n, k, x, y);
        if (std::abs(wx - x) <= tol && std::abs(wy - y) <= tol) return k;
    }
    return -1;
}

int DiagramaCuadros::ladoMasCercano(int n, double wx, double wy) const {
    double d[4] = {std::abs(wy - Y(n)), std::abs(X(n) + Wn(n) - wx), std::abs(Y(n) + Hn(n) - wy), std::abs(wx - X(n))};
    return int(std::min_element(d, d + 4) - d);
}

// Conector ortogonal con lados de salida/entrada fijados y tramo intermedio («codo») opcional
std::vector<std::pair<double, double>> DiagramaCuadros::recorridoEstilo(const Arista& a, const EstiloArista& e) const {
    auto aut = recorridoAuto(a);
    int s = e.ladoOrigen >= 0 ? e.ladoOrigen : ladoSalida(aut);
    int t = e.ladoDestino >= 0 ? e.ladoDestino : ladoEntrada(aut);
    double px, py, qx, qy;
    puntoLado(a.desde, s, e.ladoOrigen >= 0 ? e.fracOrigen : 0.5, px, py);
    puntoLado(a.hasta, t, e.ladoDestino >= 0 ? e.fracDestino : 0.5, qx, qy);
    const double stub = 22;
    std::vector<std::pair<double, double>> pts;
    // Tramo intermedio entre las dos «salidas» (stub) de los bloques; si las direcciones no lo permiten
    // (p. ej. salir por abajo hacia un bloque que no está debajo), se rodea por el hueco entre bloques.
    auto hueco = [&](bool enX) {   // coordenada libre entre los dos bloques (o por fuera de ambos)
        double a0 = enX ? X(a.desde) : Y(a.desde), a1 = a0 + (enX ? Wn(a.desde) : Hn(a.desde));
        double b0 = enX ? X(a.hasta) : Y(a.hasta), b1 = b0 + (enX ? Wn(a.hasta) : Hn(a.hasta));
        if (a1 <= b0) return (a1 + b0) / 2;
        if (b1 <= a0) return (b1 + a0) / 2;
        return std::max(a1, b1) + stub;
    };
    if (vertical(s) && vertical(t)) {
        double p1 = py + (s == 2 ? stub : -stub), q1 = qy + (t == 2 ? stub : -stub);
        double lo = -1e18, hi = 1e18;
        if (s == 2) lo = std::max(lo, p1); else hi = std::min(hi, p1);
        if (t == 2) lo = std::max(lo, q1); else hi = std::min(hi, q1);
        if (lo <= hi) {
            double m = std::clamp(e.codo ? *e.codo : (py + qy) / 2, lo, hi);
            if (lo > -1e17 && hi > 1e17 && !e.codo) m = lo;   // ambos hacia abajo: justo bajo el más bajo
            if (hi < 1e17 && lo < -1e17 && !e.codo) m = hi;   // ambos hacia arriba
            pts = {{px, py}, {px, m}, {qx, m}, {qx, qy}};
        } else {
            double xm = hueco(true);
            pts = {{px, py}, {px, p1}, {xm, p1}, {xm, q1}, {qx, q1}, {qx, qy}};
        }
    } else if (!vertical(s) && !vertical(t)) {
        double p1 = px + (s == 1 ? stub : -stub), q1 = qx + (t == 1 ? stub : -stub);
        double lo = -1e18, hi = 1e18;
        if (s == 1) lo = std::max(lo, p1); else hi = std::min(hi, p1);
        if (t == 1) lo = std::max(lo, q1); else hi = std::min(hi, q1);
        if (lo <= hi) {
            double m = std::clamp(e.codo ? *e.codo : (px + qx) / 2, lo, hi);
            if (lo > -1e17 && hi > 1e17 && !e.codo) m = lo;
            if (hi < 1e17 && lo < -1e17 && !e.codo) m = hi;
            pts = {{px, py}, {m, py}, {m, qy}, {qx, qy}};
        } else {
            double ym = hueco(false);
            pts = {{px, py}, {p1, py}, {p1, ym}, {q1, ym}, {q1, qy}, {qx, qy}};
        }
    } else if (vertical(s)) {                       // sale en vertical, entra por un lateral (desde fuera)
        double q1x = qx + (t == 1 ? stub : -stub);
        double m = e.codo ? *e.codo : (py + qy) / 2;
        pts = {{px, py}, {px, m}, {q1x, m}, {q1x, qy}, {qx, qy}};
    } else {                                        // sale por un lateral, entra en vertical (desde fuera)
        double q1y = qy + (t == 2 ? stub : -stub);
        double m = e.codo ? *e.codo : (px + qx) / 2;
        pts = {{px, py}, {m, py}, {m, q1y}, {qx, q1y}, {qx, qy}};
    }
    return pts;
}

std::vector<std::pair<double, double>> DiagramaCuadros::recorridoAuto(const Arista& ar) const {
    int desde = ar.desde, hasta = ar.hasta;
    double ax = X(desde), ay = Y(desde), bx = X(hasta), by = Y(hasta);
    double aw = Wn(desde), ah = Hn(desde), bw = Wn(hasta), bh = Hn(hasta);
    double acx = ax + aw / 2, acy = ay + ah / 2, bcx = bx + bw / 2, bcy = by + bh / 2;
    double bajo = std::max(ay + ah, by + bh) + 30;
    std::vector<std::pair<double, double>> pts;
    if (by >= ay + ah + 20) {                         // destino por debajo: sale por abajo, entra por arriba
        double my = (ay + ah + by) / 2;
        if (ar.tipo != TipoArista::Alimentacion && ar.tipo != TipoArista::AlimSAI && ar.tipo != TipoArista::AlimBat &&
            ar.tipo != TipoArista::SalidaSAI) {
            // Flechas de fuentes: cada tipo gira a su propia altura, justo debajo de la fuente,
            // para que no se solapen entre sí ni crucen la fila de cuadros
            double off = ar.tipo == TipoArista::Trafo ? 22 : ar.tipo == TipoArista::Emergencia ? 34 : ar.tipo == TipoArista::SAI ? 40 : 46;
            my = std::min(ay + ah + off, by - 10);
            // Si la bajada vertical atraviesa otro bloque, se entra al destino por un lateral
            bool choca = false;
            for (size_t i = 0; i < nodos_.size() && !choca; i++) {
                if (int(i) == desde || int(i) == hasta || !tienePos(int(i))) continue;
                double x = X(int(i)), y = Y(int(i));
                choca = bcx >= x - 4 && bcx <= x + Wn(int(i)) + 4 && y + Hn(int(i)) > my && y < by;
            }
            if (choca) {
                bool izq = acx <= bcx;
                double lado = izq ? bx - 22 : bx + bw + 22;
                pts = {{acx, ay + ah}, {acx, my}, {lado, my}, {lado, bcy}, {izq ? bx : bx + bw, bcy}};
                return pts;
            }
        }
        pts = {{acx, ay + ah}, {acx, my}, {bcx, my}, {bcx, by}};
    } else if (by + bh <= ay - 20) {                  // destino por encima
        double my = (ay + by + bh) / 2;
        pts = {{acx, ay}, {acx, my}, {bcx, my}, {bcx, by + bh}};
    } else if (bx >= acx) {                          // a la derecha
        double sx = ax + aw, ex = bx;
        if (ex < sx + 20) pts = {{acx, ay + ah}, {acx, bajo}, {bcx, bajo}, {bcx, by + bh}};
        else { double mx = (sx + ex) / 2; pts = {{sx, acy}, {mx, acy}, {mx, bcy}, {ex, bcy}}; }
    } else {                                         // a la izquierda
        double sx = ax, ex = bx + bw;
        if (ex > sx - 20) pts = {{acx, ay + ah}, {acx, bajo}, {bcx, bajo}, {bcx, by + bh}};
        else { double mx = (sx + ex) / 2; pts = {{sx, acy}, {mx, acy}, {mx, bcy}, {ex, bcy}}; }
    }
    return pts;
}

int DiagramaCuadros::hitArista(double wx, double wy) const {
    double tol = 6 / zoom_;
    for (size_t i = 0; i < aristas_.size(); i++) {
        auto pts = recorrido(aristas_[i]);
        for (size_t k = 1; k < pts.size(); k++)
            if (distSegmento(wx, wy, pts[k - 1].first, pts[k - 1].second, pts[k].first, pts[k].second) <= tol) return int(i);
    }
    return -1;
}

void DiagramaCuadros::setZoom(double z, double sx, double sy) {
    z = std::clamp(z, ZOOM_MIN, ZOOM_MAX);
    double wx, wy;
    aMundo(sx, sy, wx, wy);
    zoom_ = z;
    ox_ = sx - wx * zoom_;
    oy_ = sy - wy * zoom_;
    std::string t = std::to_string(int(std::round(zoom_ * 100))) + " %";
    gtk_button_set_label(GTK_BUTTON(zoomLabel_), t.c_str());
    gtk_widget_queue_draw(area_);
}

void DiagramaCuadros::asegurarVisible(int n, int w, int h) {
    if (n < 0 || n >= int(nodos_.size())) return;
    double m = 40;
    double x0 = X(n) * zoom_ + ox_, y0 = Y(n) * zoom_ + oy_;
    double x1 = x0 + Wn(n) * zoom_, y1 = y0 + Hn(n) * zoom_;
    if (x1 > w - m) ox_ -= x1 - (w - m);
    if (x0 < m) ox_ += m - x0;
    if (y1 > h - m) oy_ -= y1 - (h - m);
    if (y0 < m) oy_ += m - y0;
}

void DiagramaCuadros::ajustar() {
    int w = gtk_widget_get_width(area_), h = gtk_widget_get_height(area_);
    if (w <= 0 || h <= 0 || nodos_.empty()) return;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (size_t i = 0; i < nodos_.size(); i++) {
        x0 = std::min(x0, X(int(i))); y0 = std::min(y0, Y(int(i)));
        x1 = std::max(x1, X(int(i)) + Wn(int(i))); y1 = std::max(y1, Y(int(i)) + Hn(int(i)));
    }
    double m = 60;
    double z = std::clamp(std::min((w - 2 * m) / (x1 - x0), (h - 2 * m) / (y1 - y0)), ZOOM_MIN, 1.25);
    zoom_ = z;
    ox_ = (w - (x1 - x0) * z) / 2 - x0 * z;
    oy_ = (h - (y1 - y0) * z) / 2 - y0 * z;
    std::string t = std::to_string(int(std::round(zoom_ * 100))) + " %";
    gtk_button_set_label(GTK_BUTTON(zoomLabel_), t.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Selección y acciones
// ─────────────────────────────────────────────────────────────────────────────

void DiagramaCuadros::actualizarBotones() {
    bool nodo = sel_ == Sel::Nodo && indice(selA_) >= 0;
    gtk_widget_set_sensitive(btnSub_, nodo && selA_.tipo == TipoNodo::Cuadro);
    gtk_widget_set_sensitive(btnEditar_, nodo);
    gtk_widget_set_sensitive(btnEliminar_, nodo || indiceArista() >= 0);
    int ia = indiceArista();
    gtk_widget_set_visible(btnTrazado_, ia >= 0);
    gtk_widget_set_sensitive(btnTrazado_, ia >= 0 && estilo(aristas_[size_t(ia)]) != nullptr);
    gtk_widget_set_tooltip_text(btnEliminar_, sel_ == Sel::Arista ? "Desconectar la flecha seleccionada (Supr)"
                                                                  : "Eliminar el bloque seleccionado (Supr)");
}

void DiagramaCuadros::notificarSeleccion() {
    if (cb_.seleccion) cb_.seleccion(sel_ == Sel::Nodo && selA_.tipo == TipoNodo::Cuadro ? selA_.ruta : RutaCuadro{});
}

void DiagramaCuadros::selNodo(int i) {
    sel_ = Sel::Nodo; selA_ = claveDe(i); handleOwner_ = i;
    actualizarBotones();
    notificarSeleccion();
}

void DiagramaCuadros::selArista(int a) {
    sel_ = Sel::Arista;
    selA_ = claveDe(aristas_[size_t(a)].desde);
    selB_ = claveDe(aristas_[size_t(a)].hasta);
    selTipoArista_ = aristas_[size_t(a)].tipo;
    handleOwner_ = -1;
    actualizarBotones();
    notificarSeleccion();
}

void DiagramaCuadros::limpiarSel() {
    sel_ = Sel::Nada;
    actualizarBotones();
    notificarSeleccion();
}

void DiagramaCuadros::seleccionar(const RutaCuadro& r) {
    reconstruir();
    int i = indiceCuadro(r);
    if (i >= 0) { sel_ = Sel::Nodo; selA_ = claveDe(i); }
    else sel_ = Sel::Nada;
    handleOwner_ = i;
    actualizarBotones();
    gtk_widget_queue_draw(area_);
}

void DiagramaCuadros::refrescar() {
    reconstruir();
    if (sel_ == Sel::Nodo && indice(selA_) < 0) sel_ = Sel::Nada;
    if (sel_ == Sel::Arista && indiceArista() < 0) sel_ = Sel::Nada;
    hover_ = hoverHandle_ = -1;
    handleOwner_ = sel_ == Sel::Nodo ? indice(selA_) : -1;
    actualizarBotones();
    gtk_widget_queue_draw(area_);
}

void DiagramaCuadros::encuadrarAlMostrar() {
    encuadrar_ = true;
    gtk_widget_queue_draw(area_);
}

void DiagramaCuadros::nuevoCuadroGeneral() {
    reconstruir();
    double x = 40, y = 40;
    bool hayFuentes = false;
    for (size_t i = 0; i < nodos_.size(); i++) {
        if (nodos_[i].tipo != TipoNodo::Cuadro) hayFuentes = true;
        else if (nodos_[i].padre < 0) { x = std::max(x, X(int(i)) + Wn(int(i)) + GAP_X); y = Y(int(i)); }
    }
    if (hayFuentes && y < 40 + H + GAP_Y) y = 40 + H + GAP_Y;
    posicionLibre(x, y, 2);
    RutaCuadro r = crearCuadro({}, x, y);
    sel_ = Sel::Nodo; selA_ = Clave{}; selA_.ruta = r;
    if (cb_.cambioEstructura) cb_.cambioEstructura();
    notificarSeleccion();
}

void DiagramaCuadros::anadirSubcuadro() {
    reconstruir();
    int i = indice(selA_);
    if (i < 0 || sel_ != Sel::Nodo || selA_.tipo != TipoNodo::Cuadro) return;
    double x = X(i), y = Y(i) + Hn(i) + GAP_Y;
    posicionLibre(x, y, 2);
    RutaCuadro r = crearCuadro(selA_.ruta, x, y);
    selA_.ruta = r;
    if (cb_.cambioEstructura) cb_.cambioEstructura();
    notificarSeleccion();
}

void DiagramaCuadros::editarSeleccion() {
    reconstruir();
    int i = sel_ == Sel::Nodo ? indice(selA_) : -1;
    if (i < 0) return;
    const Nodo n = nodos_[size_t(i)];
    if (n.tipo == TipoNodo::Cuadro) { if (cb_.editar) cb_.editar(n.ruta); }
    else if (cb_.editarFuente) cb_.editarFuente(n.tipo, n.idx);
}

void DiagramaCuadros::eliminarSeleccion() {
    reconstruir();
    if (sel_ == Sel::Nodo) {
        int i = indice(selA_);
        if (i < 0) return;
        const Nodo n = nodos_[size_t(i)];
        if (n.tipo == TipoNodo::Cuadro) { if (cb_.eliminar) cb_.eliminar(n.ruta); }
        else if (cb_.eliminarFuente) cb_.eliminarFuente(n.tipo, n.idx);
        return;
    }
    int ia = indiceArista();
    if (ia < 0) return;
    if (!eliminarArista(ia, false)) return;
    if (cb_.cambioEstructura) cb_.cambioEstructura();
    notificarSeleccion();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Dibujo
// ─────────────────────────────────────────────────────────────────────────────

void DiagramaCuadros::datosBloque(int i, std::string& l1, std::string& l2, std::string& l3, int& estilo, bool& avisos) const {
    const Nodo& n = nodos_[size_t(i)];
    avisos = false;
    switch (n.tipo) {
        case TipoNodo::Cuadro: {
            const Cuadro* c = n.c;
            estilo = n.padre < 0 ? 0 : 1;
            l1 = c->tag; l2 = c->nombre;
            l3 = fmt(c->pKw) + " kW · " + fmt(c->sKva) + " kVA · " + std::to_string(c->cargas.size()) + " cargas";
            avisos = !c->avisos.empty();
            break;
        }
        case TipoNodo::Trafo: {
            const Transformador& t = p_->datos.transformadores[n.idx];
            estilo = 2;
            double pk = 0, qk = 0;
            for (const Cuadro& c: p_->cuadros) if (c.trafoEfectivo == t.tag) { pk += c.pKw; qk += c.qKvar; }
            double s = std::hypot(pk, qk), ref = t.sAdmTotal() > 0 ? t.sAdmTotal() : t.sNomTotal();
            l1 = t.tag;
            l2 = (t.nParalelo > 1 ? std::to_string(t.nParalelo) + " × " : "") + fmt(t.sNomKva, 0) + " kVA · " + fmt(t.uSecV, 0) + " V";
            l3 = "S " + fmt(s) + " kVA" + (ref > 0 ? " · " + fmt(100 * s / ref) + " %" : "");
            avisos = ref > 0 && s > ref;
            break;
        }
        case TipoNodo::Grupo: {
            const EquipoEmergencia& g = p_->equipos[n.idx];
            estilo = 3;
            l1 = g.tag; l2 = g.nombre.empty() ? "Grupo electrógeno" : g.nombre;
            l3 = "S emerg. " + fmt(g.sEmerKva) + " kVA" + (g.sDisenoKva ? " · grupo " + fmt(*g.sDisenoKva, 0) + " kVA" : "");
            avisos = !g.avisos.empty();
            break;
        }
        case TipoNodo::Bateria: {
            const Bateria& b = p_->cuadros[n.idx].baterias.front();
            estilo = 6;
            l1 = b.tag; l2 = "Batería de condensadores";
            l3 = b.qKvarNominal > 0 ? fmt(b.qKvarNominal, 1) + " kVAr · cos φ " + fmt(b.cosAntes, 2) + " → " + fmt(b.cosDespues, 2)
                                    : "No necesaria (cos φ " + fmt(b.cosAntes, 2) + ")";
            avisos = !b.avisos.empty();
            break;
        }
        case TipoNodo::SAI: {
            const EquipoEmergencia& u = p_->equipos[n.idx];
            estilo = 5;
            l1 = u.tag; l2 = u.nombre.empty() ? "SAI" : u.nombre;
            l3 = "S emerg. " + fmt(u.sEmerKva) + " kVA" + (u.sDisenoKva ? " · SAI " + fmt(*u.sDisenoKva, 0) + " kVA" : "") +
                 (u.capBateriaKwh ? " · " + fmt(*u.capBateriaKwh) + " kWh" : "");
            avisos = !u.avisos.empty();
            break;
        }
        case TipoNodo::FV: {
            const InstalacionFV& f = p_->datos.fotovoltaica[n.idx];
            estilo = 4;
            l1 = f.tag; l2 = f.nombre.empty() ? "Fotovoltaica" : f.nombre;
            l3 = fmt(f.pPicoKwp) + " kWp · " + fmt(f.pAcKw) + " kW AC";
            avisos = !f.avisos.empty();
            break;
        }
    }
}

void DiagramaCuadros::dibujarBloque(cairo_t* cr, int i) {
    static const Estilo* estilos[7] = {&E_ORIGEN, &E_CUADRO, &E_TRAFO, &E_GRUPO, &E_FV, &E_SAI, &E_BAT};
    const Nodo& n = nodos_[size_t(i)];
    double x = X(i), y = Y(i), w = Wn(i), h = Hn(i);
    Color txt = hex(0x1a1a1a), sub = hex(0x4a4a4a);
    std::string l1, l2, l3;
    bool avisos = false;
    int ie = 1;
    datosBloque(i, l1, l2, l3, ie, avisos);
    const Estilo* e = estilos[ie];
    // Zona de texto (las fuentes llevan su símbolo a la izquierda)
    double tx = n.tipo == TipoNodo::Cuadro ? x + 8 : x + 44;
    double ancho = n.tipo == TipoNodo::Cuadro ? w - 16 : w - 52;

    bool fuenteBloque = n.tipo != TipoNodo::Cuadro;
    rectRedondeado(cr, x, y, w, h, fuenteBloque ? 14 : 6);
    fuente(cr, e->relleno); cairo_fill_preserve(cr);
    fuente(cr, e->borde); cairo_set_line_width(cr, (fuenteBloque || n.padre < 0) ? 2 : 1.3); cairo_stroke(cr);

    // Símbolo de la fuente
    double sx = x + 24, sy = y + h / 2;
    fuente(cr, e->borde);
    cairo_set_line_width(cr, 1.8);
    if (n.tipo == TipoNodo::Trafo) {          // dos devanados
        cairo_new_sub_path(cr); cairo_arc(cr, sx, sy - 6, 9, 0, 2 * M_PI); cairo_stroke(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, sx, sy + 6, 9, 0, 2 * M_PI); cairo_stroke(cr);
    } else if (n.tipo == TipoNodo::Bateria) { // batería de condensadores: pulso (icono «charge»)
        const double px[6] = {2, 4, 6, 9, 11, 14}, py[6] = {8, 8, 2, 13, 8, 8};
        for (int k = 0; k < 6; k++) {
            double x = sx + (px[k] - 8) * 1.8, y = sy + (py[k] - 7.5) * 1.8;
            if (k == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
        }
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, 2.6); cairo_stroke(cr);
    } else if (n.tipo == TipoNodo::SAI) {     // SAI: equipo con indicador y rayo (icono «uninterruptible-power-supply»)
        rectRedondeado(cr, sx - 9, sy - 13, 18, 26, 3.5); cairo_stroke(cr);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, sx - 4.5, sy - 8.5); cairo_line_to(cr, sx + 1, sy - 8.5); cairo_stroke(cr);
        cairo_move_to(cr, sx + 2.5, sy - 5); cairo_line_to(cr, sx - 3.5, sy + 1.5); cairo_line_to(cr, sx + 0.5, sy + 1.5);
        cairo_line_to(cr, sx - 2.5, sy + 9); cairo_line_to(cr, sx + 3.5, sy + 1); cairo_line_to(cr, sx - 0.5, sy + 1);
        cairo_close_path(cr); cairo_fill(cr);
    } else if (n.tipo == TipoNodo::Grupo) {   // generador: círculo con «G~»
        cairo_new_sub_path(cr); cairo_arc(cr, sx, sy, 13, 0, 2 * M_PI); cairo_stroke(cr);
        texto(cr, "<span font_desc='Sans Bold 8'>G~</span>", sx - 13, sy - 7, 26, e->borde);
    } else if (n.tipo == TipoNodo::FV) {      // panel con celdas y sol
        cairo_rectangle(cr, sx - 13, sy - 6, 26, 18); cairo_stroke(cr);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, sx - 13, sy + 3); cairo_line_to(cr, sx + 13, sy + 3);
        cairo_move_to(cr, sx - 4.3, sy - 6); cairo_line_to(cr, sx - 4.3, sy + 12);
        cairo_move_to(cr, sx + 4.3, sy - 6); cairo_line_to(cr, sx + 4.3, sy + 12);
        cairo_stroke(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, sx + 10, sy - 13, 4, 0, 2 * M_PI); cairo_fill(cr);
    }

    // Líneas de texto que caben en la altura del bloque, centradas en vertical
    int lineas = h >= 64 ? 3 : h >= 44 ? 2 : 1;
    double y0 = y + (h - (lineas == 3 ? 58 : lineas == 2 ? 38 : 18)) / 2;
    texto(cr, "<span font_desc='Sans Bold 10'>" + esc(l1) + "</span>", tx, y0, ancho, txt);
    if (lineas >= 2) texto(cr, "<span font_desc='Sans 8'>" + esc(l2) + "</span>", tx, y0 + 20, ancho, txt);
    if (lineas >= 3) texto(cr, "<span font_desc='Sans 7.5'>" + esc(l3) + "</span>", tx, y0 + 41, ancho, sub);

    if (avisos) {
        double ax = x + w - 16, ay = y + 6;
        cairo_move_to(cr, ax + 6, ay); cairo_line_to(cr, ax + 12, ay + 11); cairo_line_to(cr, ax, ay + 11); cairo_close_path(cr);
        fuente(cr, hex(0xe5a50a)); cairo_fill(cr);
    }
}

void DiagramaCuadros::dibujar(cairo_t* cr, int w, int h) {
    reconstruir();
    if (!exportando_) {
        if (encuadrar_ && w > 0 && h > 0) { ajustar(); encuadrar_ = false; porMostrar_.reset(); }
        if (porMostrar_) { asegurarVisible(indice(*porMostrar_), w, h); porMostrar_.reset(); }
    }
    bool dark = !exportando_ && oscuro();

    // Fondo con rejilla al estilo draw.io (al exportar: fondo blanco, sin rejilla)
    fuente(cr, dark ? hex(0x1e1f22) : hex(0xffffff));
    cairo_paint(cr);
    double paso = GRID * zoom_;
    if (paso >= 5 && !exportando_) {
        cairo_set_line_width(cr, 1);
        double wx0, wy0;
        aMundo(0, 0, wx0, wy0);
        long i0 = long(std::floor(wx0 / GRID)), j0 = long(std::floor(wy0 / GRID));
        for (int pase = 0; pase < 2; pase++) {
            fuente(cr, pase == 0 ? (dark ? hex(0x26272b) : hex(0xf1f1f1)) : (dark ? hex(0x303136) : hex(0xe0e0e0)));
            for (long i = i0;; i++) {
                double sx = i * GRID * zoom_ + ox_;
                if (sx > w) break;
                if ((i % 4 == 0) != (pase == 1)) continue;
                cairo_move_to(cr, std::floor(sx) + 0.5, 0); cairo_line_to(cr, std::floor(sx) + 0.5, h);
            }
            for (long j = j0;; j++) {
                double sy = j * GRID * zoom_ + oy_;
                if (sy > h) break;
                if ((j % 4 == 0) != (pase == 1)) continue;
                cairo_move_to(cr, 0, std::floor(sy) + 0.5); cairo_line_to(cr, w, std::floor(sy) + 0.5);
            }
            cairo_stroke(cr);
        }
    }

    if (nodos_.empty()) {
        PangoLayout* l = pango_cairo_create_layout(cr);
        pango_layout_set_markup(l, "<span size='large' weight='bold'>Sin cuadros</span>\n"
                                   "Haz doble clic en el lienzo o pulsa «Cuadro general» para crear el CGBT de origen", -1);
        pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
        pango_layout_set_width(l, int((w - 40) * PANGO_SCALE));
        int lw, lh;
        pango_layout_get_pixel_size(l, &lw, &lh);
        fuente(cr, dark ? hex(0x9a9a9a) : hex(0x777777));
        cairo_move_to(cr, 20, (h - lh) / 2.0);
        pango_cairo_show_layout(cr, l);
        g_object_unref(l);
        return;
    }

    cairo_save(cr);
    cairo_translate(cr, ox_, oy_);
    cairo_scale(cr, zoom_, zoom_);

    // Flechas (conector ortogonal con punta rellena)
    Color linea = dark ? hex(0xd0d0d0) : hex(0x333333);
    int selAr = exportando_ ? -1 : indiceArista();
    for (size_t i = 0; i < aristas_.size(); i++) {
        const Arista& a = aristas_[i];
        auto pts = recorrido(a);
        bool sel = int(i) == selAr;
        Color col = linea;
        double grosor = 1.5;
        bool discontinua = false;
        switch (a.tipo) {
            case TipoArista::Trafo: col = E_TRAFO.flecha; grosor = 2.2; break;
            case TipoArista::Emergencia: col = E_GRUPO.flecha; discontinua = true; break;
            case TipoArista::SAI: col = E_SAI.flecha; discontinua = true; break;
            case TipoArista::AlimBat: col = E_BAT.flecha; grosor = 1.8; break;
            case TipoArista::SalidaSAI: col = E_SAI.flecha; grosor = 1.8; break;   // continua: el cuadro cuelga del SAI
            case TipoArista::FV: col = E_FV.flecha; discontinua = true; break;
            default: break;
        }
        if (sel) { col = AZUL_SEL; grosor += 1; }
        fuente(cr, col);
        cairo_set_line_width(cr, grosor);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        double dash[] = {7, 4};
        if (discontinua) cairo_set_dash(cr, dash, 2, 0);
        auto [ex, ey] = pts.back();
        auto [px, py] = pts[pts.size() - 2];
        double ang = std::atan2(ey - py, ex - px), L = 11, A = 0.42;
        cairo_move_to(cr, pts[0].first, pts[0].second);
        for (size_t k = 1; k + 1 < pts.size(); k++) cairo_line_to(cr, pts[k].first, pts[k].second);
        cairo_line_to(cr, ex - std::cos(ang) * L * 0.8, ey - std::sin(ang) * L * 0.8);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        cairo_move_to(cr, ex, ey);
        cairo_line_to(cr, ex - L * std::cos(ang - A), ey - L * std::sin(ang - A));
        cairo_line_to(cr, ex - L * 0.7 * std::cos(ang), ey - L * 0.7 * std::sin(ang));
        cairo_line_to(cr, ex - L * std::cos(ang + A), ey - L * std::sin(ang + A));
        cairo_close_path(cr);
        cairo_fill(cr);
    }

    // Destino de la conexión que se está arrastrando
    int destino = -1;
    bool destinoValido = false;
    bool conectando = modo_ == Modo::Conectar && movido_ && dragNodo_ >= 0 && dragNodo_ < int(nodos_.size());
    if (conectando) {
        destino = destinoSnap_;
        if (destino == dragNodo_) destino = -1;
        if (destino >= 0) destinoValido = valida(dragNodo_, destino, nullptr);
    }

    // Destino al arrastrar el extremo de una flecha
    bool extremo = modo_ == Modo::Extremo && movido_ && dragArista_ >= 0 && dragArista_ < int(aristas_.size());
    if (extremo) {
        const Arista& a = aristas_[size_t(dragArista_)];
        int fijo = dragExtremo_ == 0 ? a.hasta : a.desde, actual = dragExtremo_ == 0 ? a.desde : a.hasta;
        destino = destinoSnap_;
        if (destino == fijo) destino = -1;
        if (destino >= 0)
            destinoValido = destino == actual ||
                            valida(dragExtremo_ == 0 ? destino : a.desde, dragExtremo_ == 1 ? destino : a.hasta, nullptr);
    }

    // Bloques
    int selN = (sel_ == Sel::Nodo && !exportando_) ? indice(selA_) : -1;
    for (size_t i = 0; i < nodos_.size(); i++) {
        dibujarBloque(cr, int(i));
        double x = X(int(i)), y = Y(int(i));
        if (int(i) == destino) {
            rectRedondeado(cr, x - 4, y - 4, Wn(int(i)) + 8, Hn(int(i)) + 8, 8);
            fuente(cr, destinoValido ? hex(0x2ec27e) : hex(0xe01b24));
            cairo_set_line_width(cr, 2.5); cairo_stroke(cr);
        }
        if (int(i) == selN) {
            double dash[] = {4, 3};
            cairo_set_dash(cr, dash, 2, 0);
            rectRedondeado(cr, x - 4, y - 4, Wn(int(i)) + 8, Hn(int(i)) + 8, 8);
            fuente(cr, AZUL_SEL); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
            // Asas de redimensionado (esquinas y centros de los lados), tamaño constante en pantalla
            double r = 4 / zoom_;
            for (int k = 0; k < 8; k++) {
                double hx, hy;
                puntoResize(int(i), k, hx, hy);
                cairo_rectangle(cr, hx - r, hy - r, 2 * r, 2 * r);
                fuente(cr, hex(0xffffff)); cairo_fill_preserve(cr);
                fuente(cr, AZUL_SEL); cairo_set_line_width(cr, 1.2 / zoom_); cairo_stroke(cr);
            }
        }
    }

    // Punto de llegada ajustado (centro o centro de un lado) y puntos de conexión del bloque destino
    double finX = curWX_, finY = curWY_;
    if ((conectando || extremo) && destino >= 0) {
        double px, py;
        puntoConexion(destino, curWX_, curWY_, px, py);
        finX = px; finY = py;
        double xs[13], ys[13];
        for (int k = 0; k < 12; k++) puntoLado(destino, k / 3, FRAC_NODO[k % 3], xs[k], ys[k]);
        xs[12] = X(destino) + Wn(destino) / 2; ys[12] = Y(destino) + Hn(destino) / 2;
        double r = 3.5 / zoom_;
        cairo_set_line_width(cr, 1.5 / zoom_);
        for (int k = 0; k < 13; k++) {   // nodos de conexión: aspas «×» como en draw.io
            bool activo = (k == 12 && puntoSnap_ == -1) || k == puntoSnap_;
            fuente(cr, activo ? hex(0x2ec27e) : AZUL_SEL);
            if (activo) { cairo_new_sub_path(cr); cairo_arc(cr, xs[k], ys[k], r * 1.6, 0, 2 * M_PI); cairo_fill(cr); continue; }
            cairo_move_to(cr, xs[k] - r, ys[k] - r); cairo_line_to(cr, xs[k] + r, ys[k] + r);
            cairo_move_to(cr, xs[k] - r, ys[k] + r); cairo_line_to(cr, xs[k] + r, ys[k] - r);
            cairo_stroke(cr);
        }
    }

    // Guías de alineación al mover un bloque
    if (modo_ == Modo::Mover && (guiaX_ || guiaY_)) {
        double wx0, wy0, wx1, wy1;
        aMundo(0, 0, wx0, wy0);
        aMundo(w, h, wx1, wy1);
        double dash[] = {5 / zoom_, 4 / zoom_};
        cairo_set_dash(cr, dash, 2, 0);
        fuente(cr, hex(0x0066ff, 0.8));
        cairo_set_line_width(cr, 1 / zoom_);
        if (guiaX_) { cairo_move_to(cr, *guiaX_, wy0); cairo_line_to(cr, *guiaX_, wy1); }
        if (guiaY_) { cairo_move_to(cr, wx0, *guiaY_); cairo_line_to(cr, wx1, *guiaY_); }
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
    }

    // Vista previa de la conexión (por encima de los bloques)
    if (conectando) {
        double sx, sy;
        puntoLado(dragNodo_, dragDir_, nodoOrigen_ >= 0 ? FRAC_NODO[nodoOrigen_ % 3] : 0.5, sx, sy);
        double dash[] = {6, 4};
        cairo_set_dash(cr, dash, 2, 0);
        fuente(cr, AZUL_SEL);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, sx, sy);
        cairo_line_to(cr, finX, finY);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        TipoNodo t = nodos_[size_t(dragNodo_)].tipo;
        if (destino < 0 && (t == TipoNodo::Cuadro || t == TipoNodo::Trafo || t == TipoNodo::SAI)) {   // fantasma del cuadro que se creará
            rectRedondeado(cr, curWX_ - W / 2, curWY_ - H / 2, W, H, 6);
            fuente(cr, hex(0x29b6f2, 0.12)); cairo_fill_preserve(cr);
            cairo_set_dash(cr, dash, 2, 0); fuente(cr, AZUL_SEL); cairo_stroke(cr); cairo_set_dash(cr, nullptr, 0, 0);
        }
    }

    // Flecha seleccionada: se redibuja por encima de los bloques con sus asas de edición
    // (extremos: círculos; tramo intermedio: rombo), como en draw.io
    if (selAr >= 0 && modo_ != Modo::Extremo) {
        auto pts = recorrido(aristas_[size_t(selAr)]);
        double dash[] = {5, 4};
        cairo_set_dash(cr, dash, 2, 0);
        fuente(cr, hex(0x00a8ff, 0.8));
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, pts[0].first, pts[0].second);
        for (size_t k = 1; k < pts.size(); k++) cairo_line_to(cr, pts[k].first, pts[k].second);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        for (auto pt: {pts.front(), pts.back()}) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, pt.first, pt.second, 5, 0, 2 * M_PI);
            fuente(cr, hex(0xffffff)); cairo_fill_preserve(cr);
            fuente(cr, AZUL_SEL); cairo_set_line_width(cr, 1.8); cairo_stroke(cr);
        }
        auto c = puntoCodo(pts);
        cairo_move_to(cr, c.first, c.second - 6); cairo_line_to(cr, c.first + 6, c.second);
        cairo_line_to(cr, c.first, c.second + 6); cairo_line_to(cr, c.first - 6, c.second); cairo_close_path(cr);
        fuente(cr, hex(0xfff2a8)); cairo_fill_preserve(cr);
        fuente(cr, hex(0xb08800)); cairo_set_line_width(cr, 1.3); cairo_stroke(cr);
    }

    if (extremo) {   // línea desde el extremo fijo hasta el ratón
        auto pts = recorrido(aristas_[size_t(dragArista_)]);
        auto fijo = dragExtremo_ == 0 ? pts.back() : pts.front();
        double dash[] = {6, 4};
        cairo_set_dash(cr, dash, 2, 0);
        fuente(cr, AZUL_SEL);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, fijo.first, fijo.second);
        cairo_line_to(cr, finX, finY);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        if (destino < 0) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, finX, finY, 5, 0, 2 * M_PI);
            fuente(cr, AZUL_SEL); cairo_fill(cr);
        }
    }

    // Nodos de conexión del bloque bajo el ratón (aspas «×» como en draw.io)
    if (!exportando_ && modo_ == Modo::Nada && handleOwner_ >= 0 && handleOwner_ < int(nodos_.size())) {
        double r = 3 / zoom_;
        cairo_set_line_width(cr, 1.3 / zoom_);
        bool selec = sel_ == Sel::Nodo && indice(selA_) == handleOwner_;
        for (int k = 0; k < 12; k++) {
            if (selec && k % 3 == 1) continue;   // en el seleccionado, el centro del lado es un asa de tamaño
            double x, y;
            puntoLado(handleOwner_, k / 3, FRAC_NODO[k % 3], x, y);
            fuente(cr, k == hoverNodo_ ? hex(0x2ec27e) : hex(0x29b6f2, 0.9));
            if (k == hoverNodo_) { cairo_new_sub_path(cr); cairo_arc(cr, x, y, r * 1.6, 0, 2 * M_PI); cairo_fill(cr); continue; }
            cairo_move_to(cr, x - r, y - r); cairo_line_to(cr, x + r, y + r);
            cairo_move_to(cr, x - r, y + r); cairo_line_to(cr, x + r, y - r);
            cairo_stroke(cr);
        }
    }

    // Flechas azules de conexión del bloque bajo el ratón (o seleccionado)
    if (!exportando_ && modo_ == Modo::Nada && handleOwner_ >= 0 && handleOwner_ < int(nodos_.size())) {
        for (int d = 0; d < 4; d++) {
            double hx, hy;
            handle(handleOwner_, d, hx, hy);
            double ang = d * M_PI / 2 - M_PI / 2;   // 0: arriba, 1: derecha, 2: abajo, 3: izquierda
            double alfa = (d == hoverHandle_) ? 0.95 : 0.55;
            fuente(cr, hex(0x29b6f2, alfa));
            double ca = std::cos(ang), sa = std::sin(ang), pc = -sa, ps = ca;
            double L = H_R + 1, Wd = H_R * 0.75, s = H_R * 0.35;
            cairo_move_to(cr, hx + ca * L, hy + sa * L);
            cairo_line_to(cr, hx + pc * Wd, hy + ps * Wd);
            cairo_line_to(cr, hx + pc * s, hy + ps * s);
            cairo_line_to(cr, hx + pc * s - ca * L, hy + ps * s - sa * L);
            cairo_line_to(cr, hx - pc * s - ca * L, hy - ps * s - sa * L);
            cairo_line_to(cr, hx - pc * s, hy - ps * s);
            cairo_line_to(cr, hx - pc * Wd, hy - ps * Wd);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Eventos
// ─────────────────────────────────────────────────────────────────────────────

void DiagramaCuadros::onDraw(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
    static_cast<DiagramaCuadros*>(data)->dibujar(cr, w, h);
}

void DiagramaCuadros::onDragBegin(GtkGestureDrag*, double sx, double sy, gpointer data) {
    auto* d = static_cast<DiagramaCuadros*>(data);
    d->reconstruir();
    gtk_widget_grab_focus(d->area_);
    d->startSX_ = sx; d->startSY_ = sy; d->movido_ = false;
    double wx, wy;
    d->aMundo(sx, sy, wx, wy);
    d->curWX_ = wx; d->curWY_ = wy;

    // Asas de la flecha seleccionada: extremos y tramo intermedio
    if (d->sel_ == Sel::Arista) {
        int ia = d->indiceArista();
        if (ia >= 0) {
            auto pts = d->recorrido(d->aristas_[size_t(ia)]);
            double tol = 9 / d->zoom_;
            for (int k = 0; k < 2; k++) {
                auto pt = k == 0 ? pts.front() : pts.back();
                if (std::hypot(wx - pt.first, wy - pt.second) <= tol) {
                    d->modo_ = Modo::Extremo; d->dragArista_ = ia; d->dragExtremo_ = k;
                    return;
                }
            }
            auto c = puntoCodo(pts);
            if (std::hypot(wx - c.first, wy - c.second) <= tol) {
                d->modo_ = Modo::Codo; d->dragArista_ = ia;
                d->dragEstilo_ = d->claveEstilo(d->aristas_[size_t(ia)]);
                d->dragEstiloNuevo_ = d->estilo(d->aristas_[size_t(ia)]) == nullptr;
                EstiloArista& e = d->p_->estilosAristas[d->dragEstilo_];
                if (e.ladoOrigen < 0) e.ladoOrigen = ladoSalida(pts);
                if (e.ladoDestino < 0) e.ladoDestino = ladoEntrada(pts);
                if (!e.codo) e.codo = vertical(e.ladoOrigen) ? c.second : c.first;
                return;
            }
        }
    }

    if (d->sel_ == Sel::Nodo) {
        int n = d->indice(d->selA_);
        int k = d->hitResize(n, wx, wy);
        if (k >= 0) {
            d->modo_ = Modo::Redimensionar; d->dragNodo_ = n; d->dragResize_ = k;
            d->rx0_ = d->X(n); d->ry0_ = d->Y(n); d->rw0_ = d->Wn(n); d->rh0_ = d->Hn(n);
            return;
        }
    }

    // Nodo de conexión del bloque bajo el ratón: nueva flecha que sale de ese punto
    int nc = d->hitNodoConexion(d->handleOwner_, wx, wy);
    if (nc >= 0) {
        d->modo_ = Modo::Conectar; d->dragNodo_ = d->handleOwner_; d->dragDir_ = nc / 3; d->nodoOrigen_ = nc;
        return;
    }

    int h = d->hitHandle(d->handleOwner_, wx, wy);
    if (h >= 0) { d->nodoOrigen_ = -1; d->modo_ = Modo::Conectar; d->dragNodo_ = d->handleOwner_; d->dragDir_ = h; return; }
    int n = d->hitNodo(wx, wy);
    if (n >= 0) {
        d->modo_ = Modo::Mover; d->dragNodo_ = n;
        d->nodoX0_ = d->X(n); d->nodoY0_ = d->Y(n);
        d->selNodo(n);
        gtk_widget_set_cursor_from_name(d->area_, "grabbing");
        gtk_widget_queue_draw(d->area_);
        return;
    }
    int e = d->hitArista(wx, wy);
    if (e >= 0) { d->modo_ = Modo::Nada; d->selArista(e); gtk_widget_queue_draw(d->area_); return; }
    d->modo_ = Modo::Pan; d->panX0_ = d->ox_; d->panY0_ = d->oy_;
    if (d->sel_ != Sel::Nada) d->limpiarSel();
    d->handleOwner_ = -1;
    gtk_widget_queue_draw(d->area_);
}

void DiagramaCuadros::onDragUpdate(GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* d = static_cast<DiagramaCuadros*>(data);
    if (std::hypot(dx, dy) > 4) d->movido_ = true;
    if (!d->movido_) return;
    d->reconstruir();
    switch (d->modo_) {
        case Modo::Mover:
            if (d->dragNodo_ >= 0 && d->dragNodo_ < int(d->nodos_.size())) {
                int i = d->dragNodo_;
                double nx = d->nodoX0_ + dx / d->zoom_, ny = d->nodoY0_ + dy / d->zoom_;
                double w = d->Wn(i), h = d->Hn(i), cx = nx + w / 2, cy = ny + h / 2;
                // Ajuste al centro de otros bloques (primero los conectados), con guías como en draw.io
                double tol = 10 / d->zoom_;
                std::vector<int> conectados, resto;
                for (const Arista& a: d->aristas_) {
                    if (a.desde == i) conectados.push_back(a.hasta);
                    if (a.hasta == i) conectados.push_back(a.desde);
                }
                for (int j = 0; j < int(d->nodos_.size()); j++) if (j != i) resto.push_back(j);
                d->guiaX_.reset(); d->guiaY_.reset();
                for (const std::vector<int>* lista: {&conectados, &resto}) {
                    double mejorX = tol, mejorY = tol;
                    for (int j: *lista) {
                        double cxj = d->X(j) + d->Wn(j) / 2, cyj = d->Y(j) + d->Hn(j) / 2;
                        if (!d->guiaX_ && std::abs(cx - cxj) < mejorX) { mejorX = std::abs(cx - cxj); nx = cxj - w / 2; d->guiaX_ = cxj; }
                        if (!d->guiaY_ && std::abs(cy - cyj) < mejorY) { mejorY = std::abs(cy - cyj); ny = cyj - h / 2; d->guiaY_ = cyj; }
                    }
                }
                *d->nodos_[size_t(i)].x = d->guiaX_ ? nx : ajustarRejilla(nx);
                *d->nodos_[size_t(i)].y = d->guiaY_ ? ny : ajustarRejilla(ny);
            }
            break;
        case Modo::Pan:
            d->ox_ = d->panX0_ + dx; d->oy_ = d->panY0_ + dy;
            gtk_widget_set_cursor_from_name(d->area_, "grabbing");
            break;
        case Modo::Conectar:
        case Modo::Extremo:
            d->aMundo(d->startSX_ + dx, d->startSY_ + dy, d->curWX_, d->curWY_);
            d->actualizarSnapConexion();
            break;
        case Modo::Redimensionar:
            if (d->dragNodo_ >= 0 && d->dragNodo_ < int(d->nodos_.size())) {
                int k = d->dragResize_;
                double x0 = d->rx0_, y0 = d->ry0_, x1 = d->rx0_ + d->rw0_, y1 = d->ry0_ + d->rh0_;
                double ddx = dx / d->zoom_, ddy = dy / d->zoom_;
                if (k == 0 || k == 6 || k == 7) x0 = std::min(ajustarRejilla(x0 + ddx), x1 - W_MIN);
                if (k == 2 || k == 3 || k == 4) x1 = std::max(ajustarRejilla(x1 + ddx), x0 + W_MIN);
                if (k == 0 || k == 1 || k == 2) y0 = std::min(ajustarRejilla(y0 + ddy), y1 - H_MIN);
                if (k == 4 || k == 5 || k == 6) y1 = std::max(ajustarRejilla(y1 + ddy), y0 + H_MIN);
                const Nodo& nd = d->nodos_[size_t(d->dragNodo_)];
                *nd.x = x0; *nd.y = y0; *nd.w = x1 - x0; *nd.h = y1 - y0;
            }
            break;
        case Modo::Codo: {
            d->aMundo(d->startSX_ + dx, d->startSY_ + dy, d->curWX_, d->curWY_);
            auto it = d->p_->estilosAristas.find(d->dragEstilo_);
            if (it != d->p_->estilosAristas.end())
                it->second.codo = ajustarRejilla(vertical(it->second.ladoOrigen) ? d->curWY_ : d->curWX_);
            break;
        }
        default: break;
    }
    gtk_widget_queue_draw(d->area_);
}

void DiagramaCuadros::onDragEnd(GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* d = static_cast<DiagramaCuadros*>(data);
    d->reconstruir();
    Modo modo = d->modo_;
    d->modo_ = Modo::Nada;
    gtk_widget_set_cursor_from_name(d->area_, nullptr);

    if ((modo == Modo::Mover || modo == Modo::Redimensionar) && d->movido_) {
        if (d->cb_.cambioPosicion) d->cb_.cambioPosicion();
    } else if (modo == Modo::Codo) {
        if (d->movido_) { if (d->cb_.cambioPosicion) d->cb_.cambioPosicion(); }
        else if (d->dragEstiloNuevo_) d->p_->estilosAristas.erase(d->dragEstilo_);   // simple clic: nada cambia
    } else if (modo == Modo::Extremo && d->movido_ && d->dragArista_ >= 0 && d->dragArista_ < int(d->aristas_.size())) {
        d->aMundo(d->startSX_ + dx, d->startSY_ + dy, d->curWX_, d->curWY_);
        d->actualizarSnapConexion();
        const Arista a = d->aristas_[size_t(d->dragArista_)];
        int actual = d->dragExtremo_ == 0 ? a.desde : a.hasta, fijo = d->dragExtremo_ == 0 ? a.hasta : a.desde;
        int n = d->destinoSnap_, lado = d->puntoSnap_;   // lado -1: centro → conexión automática
        if (n == actual) {   // mismo bloque: punto de conexión (lado fijo o centro)
            const Nodo no = d->nodos_[size_t(a.desde)], nd = d->nodos_[size_t(a.hasta)];   // copias: fijarLado reconstruye
            d->fijarLado(no.tipo, no.tag, nd.tipo, nd.tag, d->dragExtremo_, lado);
            if (d->cb_.cambioPosicion) d->cb_.cambioPosicion();
        } else if (n >= 0 && n != fijo) {
            const Nodo o = d->nodos_[size_t(d->dragExtremo_ == 0 ? n : a.desde)];
            const Nodo de = d->nodos_[size_t(d->dragExtremo_ == 1 ? n : a.hasta)];
            d->reconectarArista(d->dragArista_, d->dragExtremo_, n);
            if (lado >= 0) d->fijarLado(o.tipo, o.tag, de.tipo, de.tag, d->dragExtremo_, lado);
        }
    } else if (modo == Modo::Conectar && d->dragNodo_ >= 0 && d->dragNodo_ < int(d->nodos_.size())) {
        const Nodo origen = d->nodos_[size_t(d->dragNodo_)];
        d->aMundo(d->startSX_ + dx, d->startSY_ + dy, d->curWX_, d->curWY_);
        d->actualizarSnapConexion();
        int destino = d->movido_ ? d->destinoSnap_ : -1;

        if (destino >= 0 && destino != d->dragNodo_) {
            const Nodo de = d->nodos_[size_t(destino)];
            int lado = d->puntoSnap_;
            int nodoO = d->nodoOrigen_;
            if (d->conectar(d->dragNodo_, destino)) {
                if (lado >= 0) d->fijarLado(origen.tipo, origen.tag, de.tipo, de.tag, 1, lado);
                if (nodoO >= 0) d->fijarLado(origen.tipo, origen.tag, de.tipo, de.tag, 0, nodoO);
            }
        } else if (destino < 0) {
            double x, y;
            if (!d->movido_) {   // clic en una flecha azul: nuevo bloque en esa dirección
                x = d->X(d->dragNodo_); y = d->Y(d->dragNodo_);
                switch (d->dragDir_) {
                    case 0: y -= H + GAP_Y; break;
                    case 1: x += d->Wn(d->dragNodo_) + GAP_X + 20; break;
                    case 2: y += d->Hn(d->dragNodo_) + GAP_Y; break;
                    default: x -= W + GAP_X + 20; break;
                }
                d->posicionLibre(x, y, d->dragDir_);
            } else {
                x = d->curWX_ - W / 2; y = d->curWY_ - H / 2;
            }
            if (origen.tipo == TipoNodo::Cuadro) {
                d->sel_ = Sel::Nodo; d->selA_ = Clave{};
                d->selA_.ruta = d->crearCuadro(origen.ruta, x, y);
                if (d->cb_.cambioEstructura) d->cb_.cambioEstructura();
                d->notificarSeleccion();
            } else if (origen.tipo == TipoNodo::Trafo) {   // nuevo CGBT alimentado por el transformador
                RutaCuadro r = d->crearCuadro({}, x, y);
                if (Cuadro* c = d->p_->cuadroPorRuta(r)) c->trafo = origen.tag;
                d->sel_ = Sel::Nodo; d->selA_ = Clave{}; d->selA_.ruta = r;
                if (d->cb_.cambioEstructura) d->cb_.cambioEstructura();
                d->notificarSeleccion();
            } else if (origen.tipo == TipoNodo::SAI) {   // nuevo cuadro que cuelga del SAI
                const EquipoEmergencia& u = d->p_->equipos[origen.idx];
                RutaCuadro padre;
                for (size_t k = 0; k < d->nodos_.size(); k++)
                    if (d->nodos_[k].tipo == TipoNodo::Cuadro && d->nodos_[k].tag == u.cuadroAlimentacion) padre = d->nodos_[k].ruta;
                RutaCuadro r = d->crearCuadro(padre, x, y);
                if (Cuadro* c = d->p_->cuadroPorRuta(r)) {
                    c->saiAlimentacion = u.tag; c->tagEmer = u.tag;
                    c->nombre = "CUADRO ALIMENTADO POR " + u.tag;
                }
                d->sel_ = Sel::Nodo; d->selA_ = Clave{}; d->selA_.ruta = r;
                if (d->cb_.cambioEstructura) d->cb_.cambioEstructura();
                d->notificarSeleccion();
            } else if (d->cb_.aviso) {
                d->cb_.aviso("Arrastra la flecha hasta el cuadro al que quieres conectar " + origen.tag);
            }
        }
    }
    d->dragNodo_ = -1;
    d->dragArista_ = -1;
    d->guiaX_.reset(); d->guiaY_.reset();
    d->destinoSnap_ = -1; d->puntoSnap_ = -2; d->nodoOrigen_ = -1;
    d->reconstruir();
    d->handleOwner_ = d->sel_ == Sel::Nodo ? d->indice(d->selA_) : -1;
    d->actualizarBotones();
    gtk_widget_queue_draw(d->area_);
}

void DiagramaCuadros::onPressed(GtkGestureClick*, int n, double sx, double sy, gpointer data) {
    auto* d = static_cast<DiagramaCuadros*>(data);
    if (n != 2) return;
    d->reconstruir();
    double wx, wy;
    d->aMundo(sx, sy, wx, wy);
    int i = d->hitNodo(wx, wy);
    if (i >= 0) {
        d->selNodo(i);
        d->editarSeleccion();
    } else if (d->hitArista(wx, wy) < 0 && d->hitHandle(d->handleOwner_, wx, wy) < 0) {
        double x = wx - W / 2, y = wy - H / 2;
        d->posicionLibre(x, y, 1);
        d->sel_ = Sel::Nodo; d->selA_ = Clave{};
        d->selA_.ruta = d->crearCuadro({}, x, y);
        if (d->cb_.cambioEstructura) d->cb_.cambioEstructura();
        d->notificarSeleccion();
    }
}

void DiagramaCuadros::onMotion(GtkEventControllerMotion*, double sx, double sy, gpointer data) {
    auto* d = static_cast<DiagramaCuadros*>(data);
    d->lastSX_ = sx; d->lastSY_ = sy;
    if (d->modo_ != Modo::Nada) return;
    d->reconstruir();
    double wx, wy;
    d->aMundo(sx, sy, wx, wy);
    int antesOwner = d->handleOwner_, antesH = d->hoverHandle_;

    if (d->sel_ == Sel::Arista) {
        int ia = d->indiceArista();
        if (ia >= 0) {
            auto pts = d->recorrido(d->aristas_[size_t(ia)]);
            double tol = 9 / d->zoom_;
            auto c = puntoCodo(pts);
            for (auto pt: {pts.front(), pts.back()})
                if (std::hypot(wx - pt.first, wy - pt.second) <= tol) { gtk_widget_set_cursor_from_name(d->area_, "crosshair"); return; }
            if (std::hypot(wx - c.first, wy - c.second) <= tol) {
                const EstiloArista* e = d->estilo(d->aristas_[size_t(ia)]);
                int s = e && e->ladoOrigen >= 0 ? e->ladoOrigen : ladoSalida(pts);
                gtk_widget_set_cursor_from_name(d->area_, vertical(s) ? "row-resize" : "col-resize");
                return;
            }
        }
    }

    if (d->sel_ == Sel::Nodo) {
        static const char* cursores[8] = {"nw-resize", "n-resize", "ne-resize", "e-resize", "se-resize", "s-resize", "sw-resize", "w-resize"};
        int k = d->hitResize(d->indice(d->selA_), wx, wy);
        if (k >= 0) { gtk_widget_set_cursor_from_name(d->area_, cursores[k]); return; }
    }

    int antesNodo = d->hoverNodo_;
    d->hoverNodo_ = d->hitNodoConexion(d->handleOwner_, wx, wy);
    if (d->hoverNodo_ >= 0) {
        gtk_widget_set_cursor_from_name(d->area_, "crosshair");
        if (antesNodo != d->hoverNodo_) gtk_widget_queue_draw(d->area_);
        return;
    }
    if (antesNodo >= 0) gtk_widget_queue_draw(d->area_);

    int h = d->hitHandle(d->handleOwner_, wx, wy);
    const char* cursor = nullptr;
    if (h >= 0) {
        d->hoverHandle_ = h;
        cursor = "pointer";
    } else {
        d->hoverHandle_ = -1;
        int n = d->hitNodo(wx, wy);
        d->hover_ = n;
        if (n >= 0) { d->handleOwner_ = n; cursor = "move"; }
        else {
            // Mantener las flechas mientras el ratón esté cerca del bloque
            bool cerca = false;
            if (d->handleOwner_ >= 0 && d->handleOwner_ < int(d->nodos_.size())) {
                double m = H_DIST + H_R + 4;
                double x = d->X(d->handleOwner_), y = d->Y(d->handleOwner_);
                cerca = wx >= x - m && wx <= x + d->Wn(d->handleOwner_) + m && wy >= y - m && wy <= y + d->Hn(d->handleOwner_) + m;
            }
            if (!cerca) d->handleOwner_ = d->sel_ == Sel::Nodo ? d->indice(d->selA_) : -1;
            if (d->hitArista(wx, wy) >= 0) cursor = "pointer";
        }
    }
    gtk_widget_set_cursor_from_name(d->area_, cursor);
    if (antesOwner != d->handleOwner_ || antesH != d->hoverHandle_) gtk_widget_queue_draw(d->area_);
}

void DiagramaCuadros::onLeave(GtkEventControllerMotion*, gpointer data) {
    auto* d = static_cast<DiagramaCuadros*>(data);
    if (d->modo_ != Modo::Nada) return;
    d->hover_ = d->hoverHandle_ = -1;
    d->handleOwner_ = d->sel_ == Sel::Nodo ? d->indice(d->selA_) : -1;
    gtk_widget_queue_draw(d->area_);
}

gboolean DiagramaCuadros::onScroll(GtkEventControllerScroll* ctl, double dx, double dy, gpointer data) {
    auto* d = static_cast<DiagramaCuadros*>(data);
    GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(ctl));
    if (st & GDK_CONTROL_MASK) {
        d->setZoom(d->zoom_ * std::pow(1.1, -dy), d->lastSX_, d->lastSY_);
    } else {
        if (st & GDK_SHIFT_MASK) std::swap(dx, dy);
        d->ox_ -= dx * 40; d->oy_ -= dy * 40;
        gtk_widget_queue_draw(d->area_);
    }
    return TRUE;
}

gboolean DiagramaCuadros::onKey(GtkEventControllerKey*, guint key, guint, GdkModifierType st, gpointer data) {
    auto* d = static_cast<DiagramaCuadros*>(data);
    double cx = gtk_widget_get_width(d->area_) / 2.0, cy = gtk_widget_get_height(d->area_) / 2.0;
    switch (key) {
        case GDK_KEY_Delete: case GDK_KEY_KP_Delete: case GDK_KEY_BackSpace:
            d->eliminarSeleccion(); return TRUE;
        case GDK_KEY_Escape:
            d->limpiarSel(); d->handleOwner_ = -1; gtk_widget_queue_draw(d->area_); return TRUE;
        case GDK_KEY_Return: case GDK_KEY_KP_Enter:
            if (d->sel_ == Sel::Nodo) { d->editarSeleccion(); return TRUE; }
            break;
        case GDK_KEY_plus: case GDK_KEY_KP_Add: case GDK_KEY_equal:
            if (st & GDK_CONTROL_MASK) { d->setZoom(d->zoom_ * 1.2, cx, cy); return TRUE; }
            break;
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
            if (st & GDK_CONTROL_MASK) { d->setZoom(d->zoom_ / 1.2, cx, cy); return TRUE; }
            break;
        case GDK_KEY_0: case GDK_KEY_KP_0:
            if (st & GDK_CONTROL_MASK) { d->setZoom(1.0, cx, cy); return TRUE; }
            break;
        default: break;
    }
    return FALSE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Exportación: SVG, DXF y DWG
// ─────────────────────────────────────────────────────────────────────────────

void DiagramaCuadros::extension(double& x0, double& y0, double& x1, double& y1) const {
    x0 = y0 = 1e18; x1 = y1 = -1e18;
    for (size_t i = 0; i < nodos_.size(); i++) {
        x0 = std::min(x0, X(int(i))); y0 = std::min(y0, Y(int(i)));
        x1 = std::max(x1, X(int(i)) + Wn(int(i))); y1 = std::max(y1, Y(int(i)) + Hn(int(i)));
    }
    for (const Arista& a: aristas_)
        for (auto [px, py]: recorrido(a)) { x0 = std::min(x0, px); y0 = std::min(y0, py); x1 = std::max(x1, px); y1 = std::max(y1, py); }
    if (x0 > x1) { x0 = y0 = 0; x1 = y1 = 100; }
}

bool DiagramaCuadros::exportarSVG(const std::string& ruta, std::string& error) {
    reconstruir();
    if (nodos_.empty()) { error = "El diagrama está vacío."; return false; }
    double x0, y0, x1, y1;
    extension(x0, y0, x1, y1);
    const double m = 20;
    double w = x1 - x0 + 2 * m, h = y1 - y0 + 2 * m;
    cairo_surface_t* sf = cairo_svg_surface_create(ruta.c_str(), w, h);
    cairo_svg_surface_restrict_to_version(sf, CAIRO_SVG_VERSION_1_2);
    cairo_t* cr = cairo_create(sf);

    double ox = ox_, oy = oy_, z = zoom_;
    ox_ = m - x0; oy_ = m - y0; zoom_ = 1;
    exportando_ = true;
    dibujar(cr, int(std::ceil(w)), int(std::ceil(h)));
    exportando_ = false;
    ox_ = ox; oy_ = oy; zoom_ = z;

    cairo_destroy(cr);
    cairo_surface_finish(sf);
    cairo_status_t st = cairo_surface_status(sf);
    cairo_surface_destroy(sf);
    if (st != CAIRO_STATUS_SUCCESS) { error = std::string("No se pudo escribir el SVG: ") + cairo_status_to_string(st); return false; }
    return true;
}

namespace {

// Escritor de DXF ASCII R12 (AC1009): lo abren AutoCAD, BricsCAD, LibreCAD, QCAD, DraftSight...
struct Dxf {
    std::ostringstream o;
    Dxf() { o.imbue(std::locale::classic()); o.precision(10); }
    void g(int codigo, const std::string& v) { o << codigo << "\n" << v << "\n"; }
    void g(int codigo, double v) { o << codigo << "\n" << v << "\n"; }
    void gi(int codigo, int v) { o << codigo << "\n" << v << "\n"; }

    // Texto en la codificación del DXF R12 (Windows-1252, declarada en $DWGCODEPAGE); los caracteres que
    // no existen en ella se escriben como \U+XXXX
    static std::string texto(const std::string& s) {
        std::string r;
        for (const char* p = s.c_str(); *p; p = g_utf8_next_char(p)) {
            gunichar c = g_utf8_get_char(p);
            if (c < 128) { r.push_back(char(c)); continue; }
            char u[8] = {0};
            int n = g_unichar_to_utf8(c, u);
            gsize leidos = 0, escritos = 0;
            gchar* cp = g_convert(u, n, "WINDOWS-1252", "UTF-8", &leidos, &escritos, nullptr);
            if (cp && escritos == 1) r.push_back(cp[0]);
            else { char b[16]; std::snprintf(b, sizeof b, "\\U+%04X", unsigned(c)); r += b; }
            g_free(cp);
        }
        return r;
    }

    void polilinea(const std::string& capa, const std::vector<std::pair<double, double>>& pts, bool cerrada) {
        g(0, "POLYLINE"); g(8, capa); gi(66, 1); g(10, 0.0); g(20, 0.0); g(30, 0.0); gi(70, cerrada ? 1 : 0);
        for (auto [x, y]: pts) { g(0, "VERTEX"); g(8, capa); g(10, x); g(20, -y); g(30, 0.0); }
        g(0, "SEQEND"); g(8, capa);
    }
    void linea(const std::string& capa, double x1, double y1, double x2, double y2) {
        g(0, "LINE"); g(8, capa); g(10, x1); g(20, -y1); g(30, 0.0); g(11, x2); g(21, -y2); g(31, 0.0);
    }
    void circulo(const std::string& capa, double x, double y, double r) {
        g(0, "CIRCLE"); g(8, capa); g(10, x); g(20, -y); g(30, 0.0); g(40, r);
    }
    void triangulo(const std::string& capa, double x1, double y1, double x2, double y2, double x3, double y3) {
        g(0, "SOLID"); g(8, capa);
        g(10, x1); g(20, -y1); g(30, 0.0); g(11, x2); g(21, -y2); g(31, 0.0);
        g(12, x3); g(22, -y3); g(32, 0.0); g(13, x3); g(23, -y3); g(33, 0.0);
    }
    // Rectángulo relleno (SOLID: el orden de vértices de DXF es 1-2-4-3)
    void rectanguloRelleno(const std::string& capa, double x0, double y0, double x1, double y1) {
        g(0, "SOLID"); g(8, capa);
        g(10, x0); g(20, -y0); g(30, 0.0); g(11, x1); g(21, -y0); g(31, 0.0);
        g(12, x0); g(22, -y1); g(32, 0.0); g(13, x1); g(23, -y1); g(33, 0.0);
    }
    // Texto centrado en (x, y), recortado para que quepa en «ancho»
    void textoCentrado(const std::string& capa, const std::string& s, double x, double y, double alto, double ancho) {
        std::string t = s;
        size_t maxCar = size_t(std::max(1.0, ancho / (alto * 0.7)));   // Arial: ancho medio de carácter ≈ 0,6–0,7 × altura
        if (g_utf8_strlen(t.c_str(), -1) > long(maxCar)) {
            gchar* corte = g_utf8_substring(t.c_str(), 0, long(maxCar) - 1);
            t = std::string(corte) + "...";
            g_free(corte);
        }
        g(0, "TEXT"); g(8, capa); g(10, x); g(20, -y); g(30, 0.0); g(40, alto); g(1, texto(t)); g(7, "ARIAL");
        gi(72, 1); gi(73, 2); g(11, x); g(21, -y); g(31, 0.0);
    }
};

}  // namespace

bool DiagramaCuadros::exportarDXF(const std::string& ruta, std::string& error) {
    reconstruir();
    if (nodos_.empty()) { error = "El diagrama está vacío."; return false; }
    double x0, y0, x1, y1;
    extension(x0, y0, x1, y1);

    // Capas: nombre, color ACI y tipo de línea
    struct Capa { const char* nombre; int color; const char* linea; };
    const Capa capas[] = {
        {"CGBT", 40, "CONTINUOUS"}, {"CUADROS", 5, "CONTINUOUS"}, {"TRAFOS", 1, "CONTINUOUS"},
        {"GRUPOS", 3, "CONTINUOUS"}, {"FOTOVOLTAICA", 30, "CONTINUOUS"}, {"TEXTOS", 7, "CONTINUOUS"},
        {"FLECHAS", 7, "CONTINUOUS"}, {"FLECHAS_TRAFO", 1, "CONTINUOUS"},
        {"FLECHAS_EMERGENCIA", 3, "DASHED"}, {"FLECHAS_FV", 30, "DASHED"},
        {"SAI", 6, "CONTINUOUS"}, {"FLECHAS_SAI", 6, "DASHED"},
        {"BATERIAS", 5, "CONTINUOUS"}, {"FLECHAS_BATERIA", 5, "CONTINUOUS"}, {"SALIDAS_SAI", 6, "CONTINUOUS"},
    };
    const char* capaBloque[7] = {"CGBT", "CUADROS", "TRAFOS", "GRUPOS", "FOTOVOLTAICA", "SAI", "BATERIAS"};

    Dxf d;
    d.g(0, "SECTION"); d.g(2, "HEADER");
    d.g(9, "$ACADVER"); d.g(1, "AC1009");
    d.g(9, "$DWGCODEPAGE"); d.g(3, "ANSI_1252");
    d.g(9, "$INSBASE"); d.g(10, 0.0); d.g(20, 0.0); d.g(30, 0.0);
    d.g(9, "$EXTMIN"); d.g(10, x0); d.g(20, -y1); d.g(30, 0.0);
    d.g(9, "$EXTMAX"); d.g(10, x1); d.g(20, -y0); d.g(30, 0.0);
    d.g(9, "$LTSCALE"); d.g(40, 1.0);
    d.g(9, "$TEXTSTYLE"); d.g(7, "ARIAL");
    d.g(0, "ENDSEC");

    d.g(0, "SECTION"); d.g(2, "TABLES");
    d.g(0, "TABLE"); d.g(2, "LTYPE"); d.gi(70, 2);
    d.g(0, "LTYPE"); d.g(2, "CONTINUOUS"); d.gi(70, 0); d.g(3, "Solid line"); d.gi(72, 65); d.gi(73, 0); d.g(40, 0.0);
    d.g(0, "LTYPE"); d.g(2, "DASHED"); d.gi(70, 0); d.g(3, "__ __ __"); d.gi(72, 65); d.gi(73, 2); d.g(40, 11.0);
    d.g(49, 7.0); d.g(49, -4.0);
    d.g(0, "ENDTAB");
    d.g(0, "TABLE"); d.g(2, "LAYER"); d.gi(70, int(std::size(capas)));
    for (const Capa& c: capas) { d.g(0, "LAYER"); d.g(2, c.nombre); d.gi(70, 0); d.gi(62, c.color); d.g(6, c.linea); }
    d.g(0, "ENDTAB");
    // Estilos de texto: todos los textos usan ARIAL (fuente TrueType arial.ttf)
    d.g(0, "TABLE"); d.g(2, "STYLE"); d.gi(70, 2);
    for (const char* estiloTexto: {"STANDARD", "ARIAL"}) {
        d.g(0, "STYLE"); d.g(2, estiloTexto); d.gi(70, 0); d.g(40, 0.0); d.g(41, 1.0); d.g(50, 0.0); d.gi(71, 0);
        d.g(42, 2.5); d.g(3, "arial.ttf"); d.g(4, "");
    }
    d.g(0, "ENDTAB");
    d.g(0, "ENDSEC");

    d.g(0, "SECTION"); d.g(2, "ENTITIES");
    // Flechas
    for (const Arista& a: aristas_) {
        const char* capa = a.tipo == TipoArista::Trafo ? "FLECHAS_TRAFO" : a.tipo == TipoArista::Emergencia ? "FLECHAS_EMERGENCIA"
                         : a.tipo == TipoArista::SAI ? "FLECHAS_SAI"
                         : a.tipo == TipoArista::AlimBat ? "FLECHAS_BATERIA"
                         : a.tipo == TipoArista::SalidaSAI ? "SALIDAS_SAI"
                         : a.tipo == TipoArista::FV ? "FLECHAS_FV" : "FLECHAS";
        auto pts = recorrido(a);
        auto [ex, ey] = pts.back();
        auto [px, py] = pts[pts.size() - 2];
        double ang = std::atan2(ey - py, ex - px), L = 11, A = 0.42;
        pts.back() = {ex - std::cos(ang) * L * 0.8, ey - std::sin(ang) * L * 0.8};
        d.polilinea(capa, pts, false);
        d.triangulo(capa, ex, ey, ex - L * std::cos(ang - A), ey - L * std::sin(ang - A),
                    ex - L * std::cos(ang + A), ey - L * std::sin(ang + A));
    }
    // Bloques
    for (size_t i = 0; i < nodos_.size(); i++) {
        int n = int(i);
        double x = X(n), y = Y(n), w = Wn(n), h = Hn(n);
        std::string l1, l2, l3;
        int ie = 1;
        bool avisos = false;
        datosBloque(n, l1, l2, l3, ie, avisos);
        const char* capa = capaBloque[ie];
        d.polilinea(capa, {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, true);

        TipoNodo t = nodos_[i].tipo;
        double sx = x + 24, sy = y + h / 2;
        if (t == TipoNodo::Trafo) { d.circulo(capa, sx, sy - 6, 9); d.circulo(capa, sx, sy + 6, 9); }
        else if (t == TipoNodo::Grupo) { d.circulo(capa, sx, sy, 13); d.textoCentrado(capa, "G~", sx, sy, 7, 24); }
        else if (t == TipoNodo::Bateria) {   // pulso (icono «charge»)
            const double px[6] = {2, 4, 6, 9, 11, 14}, py[6] = {8, 8, 2, 13, 8, 8};
            std::vector<std::pair<double, double>> pts;
            for (int k = 0; k < 6; k++) pts.push_back({sx + (px[k] - 8) * 1.8, sy + (py[k] - 7.5) * 1.8});
            d.polilinea(capa, pts, false);
        }
        else if (t == TipoNodo::SAI) {   // equipo con indicador y rayo
            d.polilinea(capa, {{sx - 9, sy - 13}, {sx + 9, sy - 13}, {sx + 9, sy + 13}, {sx - 9, sy + 13}}, true);
            d.linea(capa, sx - 4.5, sy - 8.5, sx + 1, sy - 8.5);
            d.polilinea(capa, {{sx + 2.5, sy - 5}, {sx - 3.5, sy + 1.5}, {sx + 0.5, sy + 1.5}, {sx - 2.5, sy + 9},
                               {sx + 3.5, sy + 1}, {sx - 0.5, sy + 1}}, true);
        }
        else if (t == TipoNodo::FV) {
            d.polilinea(capa, {{sx - 13, sy - 6}, {sx + 13, sy - 6}, {sx + 13, sy + 12}, {sx - 13, sy + 12}}, true);
            d.linea(capa, sx - 13, sy + 3, sx + 13, sy + 3);
            d.linea(capa, sx - 4.3, sy - 6, sx - 4.3, sy + 12);
            d.linea(capa, sx + 4.3, sy - 6, sx + 4.3, sy + 12);
            d.circulo(capa, sx + 10, sy - 13, 4);
        }

        double tx = t == TipoNodo::Cuadro ? x + w / 2 : x + 44 + (w - 52) / 2;
        double ancho = t == TipoNodo::Cuadro ? w - 16 : w - 52;
        int lineas = h >= 64 ? 3 : h >= 44 ? 2 : 1;
        double yc = y + h / 2, paso = 19;
        double ys = yc - paso * (lineas - 1) / 2;
        d.textoCentrado("TEXTOS", l1, tx, ys, 8, ancho);
        if (lineas >= 2) d.textoCentrado("TEXTOS", l2, tx, ys + paso, 6, ancho);
        if (lineas >= 3) d.textoCentrado("TEXTOS", l3, tx, ys + 2 * paso, 5, ancho);
    }
    d.g(0, "ENDSEC");
    d.g(0, "EOF");

    std::ofstream f(ruta, std::ios::binary | std::ios::trunc);
    if (!f) { error = "No se pudo crear el fichero «" + ruta + "»."; return false; }
    f << d.o.str();
    if (!f) { error = "Error al escribir «" + ruta + "»."; return false; }
    return true;
}

bool DiagramaCuadros::exportarDWG(const std::string& ruta, std::string& error) {
    namespace fs = std::filesystem;
    // Conversores conocidos: ODA File Converter (gratuito, de Open Design Alliance) y LibreDWG (dxf2dwg)
    gchar* oda = nullptr;
    for (const char* nombre: {"ODAFileConverter", "ODAFileConverter.AppImage", "odafileconverter"})
        if (!oda) oda = g_find_program_in_path(nombre);
    gchar* libredwg = g_find_program_in_path("dxf2dwg");

    if (!oda && !libredwg) {
        fs::path dxf = fs::path(ruta).replace_extension(".dxf");
        std::string e2;
        if (!exportarDXF(dxf.string(), e2)) { error = e2; return false; }
        error = "No hay ningún conversor a DWG instalado (ODA File Converter o LibreDWG «dxf2dwg»), así que se ha "
                "guardado el diagrama en formato DXF:\n\n" + dxf.string() +
                "\n\nAutoCAD y el resto de programas de CAD abren el DXF directamente y pueden guardarlo como DWG.";
        return false;
    }

    gchar* tmp = g_dir_make_tmp("arquitectura-XXXXXX", nullptr);
    if (!tmp) { g_free(oda); g_free(libredwg); error = "No se pudo crear una carpeta temporal."; return false; }
    fs::path dirTmp(tmp), entrada = dirTmp / "entrada", salida = dirTmp / "salida";
    g_free(tmp);
    std::error_code ec;
    fs::create_directories(entrada, ec);
    fs::create_directories(salida, ec);
    std::string base = fs::path(ruta).stem().string();
    if (base.empty()) base = "arquitectura";
    fs::path dxf = entrada / (base + ".dxf");

    bool ok = exportarDXF(dxf.string(), error);
    fs::path generado;
    if (ok) {
        std::vector<std::string> args;
        if (oda) {
            args = {oda, entrada.string(), salida.string(), "ACAD2018", "DWG", "0", "1", "*.DXF"};
            generado = salida / (base + ".dwg");
        } else {
            generado = salida / (base + ".dwg");
            args = {libredwg, "-y", "-o", generado.string(), dxf.string()};
        }
        std::vector<char*> argv;
        for (std::string& a: args) argv.push_back(a.data());
        argv.push_back(nullptr);
        gchar *out = nullptr, *err = nullptr;
        gint estado = 0;
        GError* gerr = nullptr;
        bool lanzado = g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, &out, &err, &estado, &gerr);
        if (!lanzado) { error = std::string("No se pudo ejecutar el conversor: ") + (gerr ? gerr->message : "?"); ok = false; }
        else if (!fs::exists(generado)) {
            error = "El conversor no generó el DWG.";
            if (err && *err) error += std::string("\n\n") + err;
            ok = false;
        } else {
            fs::copy_file(generado, ruta, fs::copy_options::overwrite_existing, ec);
            if (ec) { error = "No se pudo copiar el DWG a «" + ruta + "»: " + ec.message(); ok = false; }
        }
        if (gerr) g_error_free(gerr);
        g_free(out); g_free(err);
    }
    fs::remove_all(dirTmp, ec);
    g_free(oda); g_free(libredwg);
    return ok;
}

void DiagramaCuadros::dialogoExportar(int formato) {
    reconstruir();
    if (nodos_.empty()) { if (cb_.aviso) cb_.aviso("El diagrama está vacío"); return; }
    static const char* ext[3] = {"svg", "dxf", "dwg"};
    static const char* nombre[3] = {"Imagen SVG (*.svg)", "Dibujo DXF (*.dxf)", "Dibujo DWG (*.dwg)"};
    GtkFileDialog* fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Exportar arquitectura eléctrica");
    GtkFileFilter* filtro = gtk_file_filter_new();
    gtk_file_filter_set_name(filtro, nombre[formato]);
    gtk_file_filter_add_suffix(filtro, ext[formato]);
    GListStore* filtros = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filtros, filtro);
    g_object_unref(filtro);
    gtk_file_dialog_set_filters(fd, G_LIST_MODEL(filtros));
    g_object_unref(filtros);
    gtk_file_dialog_set_initial_name(fd, (std::string("arquitectura.") + ext[formato]).c_str());
    g_object_set_data(G_OBJECT(fd), "formato", GINT_TO_POINTER(formato));

    GtkWindow* ventana = GTK_WINDOW(gtk_widget_get_root(area_));
    gtk_file_dialog_save(fd, ventana, nullptr, [](GObject* src, GAsyncResult* r, gpointer data) {
        auto* d = static_cast<DiagramaCuadros*>(data);
        int formato = GPOINTER_TO_INT(g_object_get_data(src, "formato"));
        GFile* f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), r, nullptr);
        if (!f) return;
        char* p = g_file_get_path(f);
        g_object_unref(f);
        if (!p) return;
        std::string ruta = p;
        g_free(p);
        std::string sufijo = std::string(".") + ext[formato];
        std::string actual = std::filesystem::path(ruta).extension().string();
        std::transform(actual.begin(), actual.end(), actual.begin(), ::tolower);
        if (actual != sufijo) ruta += sufijo;

        std::string error;
        bool ok = formato == 0 ? d->exportarSVG(ruta, error) : formato == 1 ? d->exportarDXF(ruta, error) : d->exportarDWG(ruta, error);
        if (ok) {
            if (d->cb_.aviso) d->cb_.aviso("Diagrama exportado a " + std::filesystem::path(ruta).filename().string());
        } else {
            AdwAlertDialog* al = ADW_ALERT_DIALOG(adw_alert_dialog_new(formato == 2 ? "Exportación a DWG" : "No se pudo exportar", error.c_str()));
            adw_alert_dialog_add_response(al, "ok", "Aceptar");
            adw_alert_dialog_set_close_response(al, "ok");
            adw_dialog_present(ADW_DIALOG(al), d->area_);
        }
    }, this);
    g_object_unref(fd);
}
