#include <adwaita.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>

#include "diagrama.hpp"
#include "iconos.h"
#include "logo_svg.h"
#include "excel.hpp"
#include "model.hpp"

struct DlgCuadro;

struct AppState {
    GtkApplication* app = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* summary = nullptr;
    // Pestaña «General»
    GtkWidget* listaTrafos = nullptr;
    GtkWidget* listaGE = nullptr;
    GtkWidget* listaSAI = nullptr;
    GtkWidget* listaFV = nullptr;
    GtkWidget* listaZonas = nullptr;
    GtkWidget* tablaTipos = nullptr;    // GtkColumnView con los tipos de carga (sólo con su ventana abierta)
    AdwToastOverlay* toastsTipos = nullptr;
    GtkWidget* resumenTipos = nullptr;  // fila de Datos generales con el nº de tipos
    DlgCuadro* dlgCuadro = nullptr;     // diálogo de cuadro abierto (para refrescar su lista de cargas)
    // Pestaña «Cuadros y cargas»
    GtkWidget* archView = nullptr;      // GtkColumnView con el árbol
    GtkWidget* archStack = nullptr;     // GtkStack: «arbol» / «vacio»
    GtkWidget* archResumen = nullptr;   // línea de resumen sobre el árbol
    // Pestaña «Arquitectura eléctrica»
    DiagramaCuadros* diagrama = nullptr;
    AdwToastOverlay* toasts = nullptr;
    std::vector<size_t> rutaSeleccionada;   // cuadro de destino por defecto para nuevas cargas
    std::string ultimoTipoCarga = "MOTOR";
    // Deshacer / rehacer: instantáneas del proyecto (JSON)
    std::vector<std::string> deshacer, rehacer;
    std::string estadoActual;
    bool restaurando = false;
    Proyecto proyecto;
    std::string ruta;
    std::string rutaInicial;
    bool modificado = false;
};

static std::string fmt(double v, int decimals = 2) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(decimals) << v;
    std::string r = s.str();
    for (char& c: r) if (c == '.') c = ',';
    return r;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// «GE1 — Nombre (grupo electrógeno)» / «UPS1 — Nombre (SAI)»
static std::string etiqueta_equipo(const EquipoEmergencia& e) {
    std::string t = e.tipo == "UPS" ? "SAI" : e.tipo == "G. ELEC." ? "grupo electrógeno" : e.tipo;
    return e.tag + (e.nombre.empty() ? "" : " — " + e.nombre) + " (" + t + ")";
}

static std::string unir(const std::vector<std::string>& v, const char* sep = "\n") {
    std::string r;
    for (size_t i = 0; i < v.size(); i++) { if (i) r += sep; r += v[i]; }
    return r;
}

static void recompute(AppState* a) {
    a->proyecto.recalcular();
    if (a->summary) {
        std::ostringstream s;
        s << "RESUMEN ELÉCTRICO\n\n";
        for (const Cuadro& c: a->proyecto.cuadros) {
            s << c.tag << "  -  " << c.nombre << "\n"
              << "  P demandada: " << fmt(c.pKw) << " kW\n"
              << "  Q demandada: " << fmt(c.qKvar) << " kVAr\n"
              << "  S demandada: " << fmt(c.sKva) << " kVA\n"
              << "  cos φ: " << fmt(c.cosPhi,3) << "\n\n";
        }
        s << "AVISOS (" << a->proyecto.avisos.size() << ")\n";
        for (const std::string& aviso: a->proyecto.avisos) s << "• " << aviso << "\n";
        gtk_label_set_text(GTK_LABEL(a->summary), s.str().c_str());
    }
}

static void show_error(AppState* a, const std::string& message) {
    AdwAlertDialog* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Error", message.c_str()));
    adw_alert_dialog_add_response(dialog, "close", "Cerrar");
    adw_alert_dialog_set_default_response(dialog, "close");
    adw_alert_dialog_set_close_response(dialog, "close");
    adw_dialog_present(ADW_DIALOG(dialog), a->window);
}

static void show_toast(AdwToastOverlay* overlay, const std::string& message) {
    if (!overlay) return;
    char* esc = g_markup_escape_text(message.c_str(), -1);
    adw_toast_overlay_add_toast(overlay, adw_toast_new(esc));
    g_free(esc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Pestaña «Cuadros y cargas»: árbol CGBT (origen) → subcuadros → cargas
// ─────────────────────────────────────────────────────────────────────────────

enum class Nodo { Grupo, Cuadro, Carga, Bateria, Equipo };

enum Col { C_ELEMENTO, C_REV, C_DESCRIPCION, C_TIPO, C_FASES, C_LONG, C_PINST, C_PDEM, C_QDEM, C_SDEM, C_COS, C_KU, C_KS, C_KE, C_EMER, C_AVISOS, N_COLS };

struct ArchNode {
    Nodo tipo = Nodo::Grupo;
    std::vector<size_t> ruta;            // cuadro al que pertenece (o que es) el nodo
    int idx = -1;                        // índice de la carga dentro de su cuadro
    std::string icono;
    std::array<std::string, N_COLS> col;
    std::string avisos;
};

static GObject* nuevo_nodo(ArchNode* n) {
    GObject* o = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
    g_object_set_data_full(o, "nodo", n, [](gpointer p) { delete static_cast<ArchNode*>(p); });
    return o;
}

static void anadir_hijo(GObject* padre, GObject* hijo) {
    GListStore* s = static_cast<GListStore*>(g_object_get_data(padre, "hijos"));
    if (!s) {
        s = g_list_store_new(G_TYPE_OBJECT);
        g_object_set_data_full(padre, "hijos", s, g_object_unref);
    }
    g_list_store_append(s, hijo);
    g_object_unref(hijo);
}

static GListModel* crear_hijos(gpointer item, gpointer) {
    GListStore* s = static_cast<GListStore*>(g_object_get_data(G_OBJECT(item), "hijos"));
    if (!s || g_list_model_get_n_items(G_LIST_MODEL(s)) == 0) return nullptr;
    return G_LIST_MODEL(g_object_ref(s));
}

static ArchNode* nodo_de_fila(gpointer treeRow) {
    if (!treeRow) return nullptr;
    GObject* item = G_OBJECT(gtk_tree_list_row_get_item(GTK_TREE_LIST_ROW(treeRow)));
    if (!item) return nullptr;
    ArchNode* n = static_cast<ArchNode*>(g_object_get_data(item, "nodo"));
    g_object_unref(item);   // el nodo sigue vivo: lo retiene el modelo
    return n;
}

static ArchNode* nodo_vacio(Nodo tipo, const std::string& icono) {
    ArchNode* n = new ArchNode;
    n->tipo = tipo; n->icono = icono;
    n->col.fill("");
    return n;
}

static bool es_sai(const EquipoEmergencia& e) { return e.tipo == "UPS"; }

// Equipo de emergencia como fila del árbol (idx: índice en proyecto.equipos)
static GObject* nodo_equipo(const EquipoEmergencia& e, size_t idx) {
    bool sai = es_sai(e);
    ArchNode* n = nodo_vacio(Nodo::Equipo, sai ? "uninterruptible-power-supply-symbolic" : "network-cellular-gprs-symbolic");
    n->idx = int(idx);
    std::string desc = e.nombre + (e.nombre.empty() ? "" : " · ") + std::to_string(e.nConsumidores) + " consumidor(es)";
    if (e.sDisenoKva) desc += " · " + std::string(sai ? "SAI de " : "grupo de ") + fmt(*e.sDisenoKva, 1) + " kVA";
    if (sai && e.capBateriaKwh) desc += " · baterías " + fmt(*e.capBateriaKwh) + " kWh";
    n->col[C_ELEMENTO] = e.tag;
    n->col[C_DESCRIPCION] = desc;
    n->col[C_TIPO] = sai ? "SAI" : e.tipo == "G. ELEC." ? "GRUPO ELECTRÓGENO" : e.tipo;
    n->col[C_FASES] = e.fases + " / " + fmt(e.tensionV, 0) + " V";
    n->col[C_SDEM] = fmt(e.sEmerKva);
    n->avisos = unir(e.avisos);
    return nuevo_nodo(n);
}

// Cuadros en los que se muestra un equipo de emergencia: el SAI, en el cuadro que lo alimenta; el grupo
// electrógeno (o un SAI sin cuadro de alimentación), en los cuadros a los que se conecta
static std::vector<std::string> cuadros_de_equipo(const Proyecto& p, const EquipoEmergencia& e) {
    if (es_sai(e) && !e.cuadroAlimentacion.empty()) return {e.cuadroAlimentacion};
    std::vector<std::string> r, porCargas;
    std::function<void(const std::vector<Cuadro>&)> v = [&](const std::vector<Cuadro>& cs) {
        for (const Cuadro& c: cs) {
            if (c.tagEmer && *c.tagEmer == e.tag) r.push_back(c.tag);
            else if (std::any_of(c.cargas.begin(), c.cargas.end(), [&](const Carga& k) { return k.tagEmer && *k.tagEmer == e.tag; }))
                porCargas.push_back(c.tag);
            v(c.subcuadros);
        }
    };
    v(p.cuadros);
    return r.empty() ? porCargas : r;
}

static GObject* nodo_cuadro(const Cuadro& c, const std::vector<size_t>& ruta, const Proyecto& p) {
    const DatosGenerales& dg = p.datos;
    ArchNode* n = nodo_vacio(Nodo::Cuadro, "view-grid-symbolic");
    n->ruta = ruta;
    n->col[C_ELEMENTO] = c.tag; n->col[C_DESCRIPCION] = c.nombre; n->col[C_TIPO] = c.tipo;
    n->col[C_FASES] = c.configFases + " / " + fmt(c.tensionV, 0) + " V";
    n->col[C_PINST] = fmt(c.pInstKw); n->col[C_PDEM] = fmt(c.pKw); n->col[C_QDEM] = fmt(c.qKvar);
    n->col[C_SDEM] = fmt(c.sKva); n->col[C_COS] = fmt(c.cosPhi, 3);
    n->col[C_KS] = fmt(c.ksGlobal, 2);   // simultaneidad global del cuadro
    n->col[C_KE] = fmt(c.ke, 2);
    n->col[C_EMER] = c.tagEmer.value_or("");
    n->avisos = unir(c.avisos);
    GObject* o = nuevo_nodo(n);

    for (size_t k = 0; k < c.cargas.size(); k++) {
        const Carga& carga = c.cargas[k];
        ArchNode* h = nodo_vacio(Nodo::Carga, "plug-symbolic");
        h->ruta = ruta; h->idx = int(k);
        h->col[C_ELEMENTO] = carga.tag; h->col[C_REV] = carga.rev; h->col[C_DESCRIPCION] = carga.nombre; h->col[C_TIPO] = dg.descripcionTipo(carga.tipo);
        h->col[C_FASES] = carga.fases + " / " + fmt(carga.tensionV, 0) + " V";
        h->col[C_LONG] = carga.longitudM ? fmt(*carga.longitudM, 1) : "";
        h->col[C_PINST] = fmt(carga.pNomKw); h->col[C_PDEM] = fmt(carga.pKw); h->col[C_QDEM] = fmt(carga.qKvar);
        h->col[C_SDEM] = fmt(carga.sKva); h->col[C_COS] = fmt(carga.cosEfectivo, 3);
        h->col[C_KU] = fmt(carga.ku, 2); h->col[C_KS] = fmt(carga.ks, 2); h->col[C_KE] = fmt(carga.ke, 2);
        h->col[C_EMER] = carga.tagEmer.value_or("");
        h->avisos = unir(carga.avisos);
        anadir_hijo(o, nuevo_nodo(h));
    }
    // Equipos del cuadro (batería de condensadores y equipos de emergencia), en un desplegable aparte
    GObject* equipos = nullptr;
    auto grupoEquipos = [&]() {
        if (!equipos) {
            ArchNode* g = nodo_vacio(Nodo::Grupo, "applications-system-symbolic");
            g->ruta = ruta;
            g->col[C_ELEMENTO] = "Equipos";
            g->col[C_DESCRIPCION] = "Batería de condensadores y equipos de emergencia";
            equipos = nuevo_nodo(g);
        }
        return equipos;
    };
    for (const Bateria& b: c.baterias) {
        ArchNode* h = nodo_vacio(Nodo::Bateria, "charge-symbolic");
        h->ruta = ruta;
        h->col[C_ELEMENTO] = b.tag; h->col[C_DESCRIPCION] = b.nombre; h->col[C_TIPO] = dg.descripcionTipo(TIPO_BATERIA);
        h->col[C_FASES] = b.configFases + " / " + fmt(b.tensionV(), 0) + " V";
        h->col[C_QDEM] = fmt(b.qKvar());
        h->col[C_COS] = b.qKvarNominal > 0 ? "→ " + fmt(b.cosDespues, 3) : "";
        h->avisos = unir(b.avisos);
        anadir_hijo(grupoEquipos(), nuevo_nodo(h));
    }
    // Equipos de emergencia conectados a este cuadro
    for (size_t k = 0; k < p.equipos.size(); k++) {
        auto tags = cuadros_de_equipo(p, p.equipos[k]);
        if (std::find(tags.begin(), tags.end(), c.tag) != tags.end()) anadir_hijo(grupoEquipos(), nodo_equipo(p.equipos[k], k));
    }
    if (equipos) anadir_hijo(o, equipos);
    for (size_t i = 0; i < c.subcuadros.size(); i++) {
        std::vector<size_t> r = ruta; r.push_back(i);
        anadir_hijo(o, nodo_cuadro(c.subcuadros[i], r, p));
    }
    return o;
}

static AppState* g_app = nullptr;   // la aplicación tiene un único estado
static void refresh_all(AppState* a);

static void on_arch_selection(GtkSelectionModel* sel, guint, guint, gpointer data) {
    AppState* a = static_cast<AppState*>(data);
    GtkBitset* b = gtk_selection_model_get_selection(sel);
    if (!gtk_bitset_is_empty(b)) {
        GObject* row = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(sel), gtk_bitset_get_minimum(b)));
        ArchNode* n = nodo_de_fila(row);
        if (n && (n->tipo == Nodo::Cuadro || n->tipo == Nodo::Carga || n->tipo == Nodo::Bateria))
            a->rutaSeleccionada = n->ruta;
        if (row) g_object_unref(row);
    }
    gtk_bitset_unref(b);
}

// Revisión de una carga, editada directamente en la lista («cuadro/ruta|índice»)
struct CambioRev { AppState* a; std::string clave, valor; };
static std::string clave_carga(const std::vector<size_t>& ruta, int idx) {
    std::string r;
    for (size_t i: ruta) r += std::to_string(i) + "/";
    return r + "|" + std::to_string(idx);
}

static void conectar_clic(gpointer boton, std::function<void()> fn, const char* senal = "clicked") {
    g_signal_connect_data(boton, senal,
        G_CALLBACK(+[](GtkButton*, gpointer f) { (*static_cast<std::function<void()>*>(f))(); }),
        new std::function<void()>(std::move(fn)),
        [](gpointer f, GClosure*) { delete static_cast<std::function<void()>*>(f); }, GConnectFlags(0));
}

// Igual que conectar_clic, para señales «notify::…» (cuyo manejador recibe además el GParamSpec)
static void conectar_notify(gpointer obj, const char* senal, std::function<void()> fn) {
    g_signal_connect_data(obj, senal,
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer f) { (*static_cast<std::function<void()>*>(f))(); }),
        new std::function<void()>(std::move(fn)),
        [](gpointer f, GClosure*) { delete static_cast<std::function<void()>*>(f); }, GConnectFlags(0));
}

// Fila de lista de solo lectura (título, subtítulo, valores a la derecha y avisos)
static GtkWidget* fila_info(const std::string& titulo, const std::string& subtitulo,
                            const std::string& valores, const std::string& avisos) {
    GtkWidget* r = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), titulo.c_str());
    if (!subtitulo.empty()) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), subtitulo.c_str());
    if (!avisos.empty()) {
        GtkWidget* img = gtk_image_new_from_icon_name("dialog-warning-symbolic");
        gtk_widget_add_css_class(img, "warning");
        gtk_widget_set_tooltip_text(img, avisos.c_str());
        adw_action_row_add_prefix(ADW_ACTION_ROW(r), img);
    }
    if (!valores.empty()) {
        GtkWidget* lbl = gtk_label_new(valores.c_str());
        gtk_widget_add_css_class(lbl, "numeric");
        gtk_widget_add_css_class(lbl, "dim-label");
        adw_action_row_add_suffix(ADW_ACTION_ROW(r), lbl);
    }
    return r;
}

static GtkWidget* fila_vacia(const char* texto) {
    GtkWidget* r = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), texto);
    gtk_widget_add_css_class(r, "dim-label");
    return r;
}

static void vaciar_lista(GtkWidget* lista) {
    while (GtkWidget* c = gtk_widget_get_first_child(lista)) gtk_list_box_remove(GTK_LIST_BOX(lista), c);
}

static GtkWidget* nueva_lista() {
    GtkWidget* l = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(l), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(l, "boxed-list");
    return l;
}

static void refresh_cuadros(AppState* a) {
    if (!a->archView) return;
    const Proyecto& p = a->proyecto;
    GListStore* raiz = g_list_store_new(G_TYPE_OBJECT);

    // Los cuadros raíz (CGBT) son los orígenes de la instalación
    for (size_t i = 0; i < p.cuadros.size(); i++) {
        GObject* o = nodo_cuadro(p.cuadros[i], {i}, p);
        g_list_store_append(raiz, o);
        g_object_unref(o);
    }

    // Equipos de emergencia que no están conectados a ningún cuadro
    {
        GObject* g = nullptr;
        for (size_t k = 0; k < p.equipos.size(); k++) {
            if (!cuadros_de_equipo(p, p.equipos[k]).empty()) continue;
            if (!g) {
                ArchNode* gn = nodo_vacio(Nodo::Grupo, "dialog-question-symbolic");
                gn->col[C_ELEMENTO] = "SIN CONECTAR";
                gn->col[C_DESCRIPCION] = "Equipos de emergencia sin cuadro asociado";
                g = nuevo_nodo(gn);
            }
            anadir_hijo(g, nodo_equipo(p.equipos[k], k));
        }
        if (g) { g_list_store_append(raiz, g); g_object_unref(g); }
    }

    GtkTreeListModel* tree = gtk_tree_list_model_new(G_LIST_MODEL(raiz), FALSE, TRUE, crear_hijos, nullptr, nullptr);
    GtkMultiSelection* sel = gtk_multi_selection_new(G_LIST_MODEL(tree));   // varias cargas: Ctrl/Mayús + clic

    // Mantener seleccionado el cuadro de trabajo tras reconstruir el árbol
    if (!a->rutaSeleccionada.empty()) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(tree));
        for (guint i = 0; i < n; i++) {
            GObject* row = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(tree), i));
            ArchNode* nodo = nodo_de_fila(row);
            g_object_unref(row);
            if (nodo && nodo->tipo == Nodo::Cuadro && nodo->ruta == a->rutaSeleccionada) {
                gtk_selection_model_select_item(GTK_SELECTION_MODEL(sel), i, TRUE);
                break;
            }
        }
    }
    g_signal_connect(sel, "selection-changed", G_CALLBACK(on_arch_selection), a);
    gtk_column_view_set_model(GTK_COLUMN_VIEW(a->archView), GTK_SELECTION_MODEL(sel));
    g_object_unref(sel);

    size_t nCuadros = 0, nCargas = 0;
    double pk = 0, qk = 0;
    std::function<void(const std::vector<Cuadro>&)> contar = [&](const std::vector<Cuadro>& cs) {
        for (const Cuadro& c: cs) { nCuadros++; nCargas += c.cargas.size(); contar(c.subcuadros); }
    };
    contar(p.cuadros);
    for (const Cuadro& c: p.cuadros) { pk += c.pKw; qk += c.qKvar; }
    std::string resumen = std::to_string(p.cuadros.size()) + " origen(es) · " +
                          std::to_string(nCuadros) + " cuadro(s) · " + std::to_string(nCargas) + " carga(s) · " +
                          "P = " + fmt(pk) + " kW · S = " + fmt(std::hypot(pk, qk)) + " kVA";
    gtk_label_set_text(GTK_LABEL(a->archResumen), resumen.c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(a->archStack), p.cuadros.empty() ? "vacio" : "arbol");
}

static void col_setup(GtkSignalListItemFactory*, GObject* obj, gpointer data) {
    int col = GPOINTER_TO_INT(data);
    GtkListItem* li = GTK_LIST_ITEM(obj);
    if (col == C_ELEMENTO) {
        GtkWidget* exp = gtk_tree_expander_new();
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget* img = gtk_image_new();
        GtkWidget* lbl = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_box_append(GTK_BOX(box), img);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_tree_expander_set_child(GTK_TREE_EXPANDER(exp), box);
        gtk_list_item_set_child(li, exp);
    } else if (col == C_AVISOS) {
        GtkWidget* img = gtk_image_new_from_icon_name("dialog-warning-symbolic");
        gtk_widget_add_css_class(img, "warning");
        gtk_list_item_set_child(li, img);
    } else if (col == C_REV) {   // la revisión la rellena el usuario aquí
        GtkWidget* el = gtk_editable_label_new("");
        g_signal_connect(el, "notify::editing", G_CALLBACK(+[](GtkEditableLabel* el, GParamSpec*, gpointer) {
            if (gtk_editable_label_get_editing(el) || !g_app) return;
            const char* clave = static_cast<const char*>(g_object_get_data(G_OBJECT(el), "carga"));
            const char* orig = static_cast<const char*>(g_object_get_data(G_OBJECT(el), "original"));
            std::string texto = trim(gtk_editable_get_text(GTK_EDITABLE(el)));
            if (!clave || (orig && texto == orig)) return;
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
                auto* c = static_cast<CambioRev*>(p);
                std::string ruta = c->clave.substr(0, c->clave.find('|'));
                int idx = std::atoi(c->clave.substr(c->clave.find('|') + 1).c_str());
                std::vector<size_t> r;
                for (size_t i = 0, j; (j = ruta.find('/', i)) != std::string::npos; i = j + 1) r.push_back(size_t(std::atol(ruta.substr(i, j - i).c_str())));
                Cuadro* cu = c->a->proyecto.cuadroPorRuta(r);
                if (cu && idx >= 0 && size_t(idx) < cu->cargas.size()) {
                    cu->cargas[size_t(idx)].rev = c->valor;
                    c->a->modificado = true;
                    refresh_all(c->a);
                }
                return G_SOURCE_REMOVE;
            }, new CambioRev{g_app, clave, texto}, [](gpointer p) { delete static_cast<CambioRev*>(p); });
        }), nullptr);
        gtk_list_item_set_child(li, el);
    } else {
        GtkWidget* lbl = gtk_label_new("");
        bool numerica = col >= C_LONG && col <= C_KE;
        gtk_label_set_xalign(GTK_LABEL(lbl), numerica ? 1.0f : 0.0f);
        if (numerica) gtk_widget_add_css_class(lbl, "numeric");
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_list_item_set_child(li, lbl);
    }
}

static void col_bind(GtkSignalListItemFactory*, GObject* obj, gpointer data) {
    int col = GPOINTER_TO_INT(data);
    GtkListItem* li = GTK_LIST_ITEM(obj);
    GtkTreeListRow* row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(li));
    ArchNode* n = nodo_de_fila(row);
    if (!n) return;
    bool destacado = n->tipo == Nodo::Grupo || n->tipo == Nodo::Cuadro;
    GtkWidget* child = gtk_list_item_get_child(li);

    if (col == C_ELEMENTO) {
        gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(child), row);
        GtkWidget* box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(child));
        GtkWidget* img = gtk_widget_get_first_child(box);
        GtkWidget* lbl = gtk_widget_get_next_sibling(img);
        gtk_image_set_from_icon_name(GTK_IMAGE(img), n->icono.c_str());
        gtk_label_set_text(GTK_LABEL(lbl), n->col[C_ELEMENTO].c_str());
        if (destacado) gtk_widget_add_css_class(lbl, "heading"); else gtk_widget_remove_css_class(lbl, "heading");
    } else if (col == C_AVISOS) {
        gtk_widget_set_visible(child, !n->avisos.empty());
        gtk_widget_set_tooltip_text(child, n->avisos.empty() ? nullptr : n->avisos.c_str());
    } else if (col == C_REV) {
        bool carga = n->tipo == Nodo::Carga;
        gtk_editable_set_text(GTK_EDITABLE(child), carga ? n->col[C_REV].c_str() : "");
        g_object_set_data_full(G_OBJECT(child), "original", g_strdup(n->col[C_REV].c_str()), g_free);
        g_object_set_data_full(G_OBJECT(child), "carga", carga ? g_strdup(clave_carga(n->ruta, n->idx).c_str()) : nullptr, g_free);
        gtk_widget_set_visible(child, carga);
        gtk_widget_set_tooltip_text(child, carga ? "Revisión de la carga: haz clic para editarla" : nullptr);
    } else {
        gtk_label_set_text(GTK_LABEL(child), n->col[col].c_str());
        gtk_widget_set_tooltip_text(child, n->col[col].empty() ? nullptr : n->col[col].c_str());
        if (destacado && col >= C_PINST && col <= C_KE) gtk_widget_add_css_class(child, "heading");
        else gtk_widget_remove_css_class(child, "heading");
    }
}

static GtkColumnViewColumn* nueva_columna(GtkColumnView* cv, const char* titulo, int col, int ancho, bool expandir = false) {
    GtkListItemFactory* f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(col_setup), GINT_TO_POINTER(col));
    g_signal_connect(f, "bind", G_CALLBACK(col_bind), GINT_TO_POINTER(col));
    GtkColumnViewColumn* c = gtk_column_view_column_new(titulo, f);
    gtk_column_view_column_set_resizable(c, TRUE);
    if (ancho > 0) gtk_column_view_column_set_fixed_width(c, ancho);
    gtk_column_view_column_set_expand(c, expandir);
    gtk_column_view_append_column(cv, c);
    g_object_unref(c);
    return c;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Diálogo «Nueva carga»
// ─────────────────────────────────────────────────────────────────────────────

static void refresh_all(AppState* a);
static void refresh_general(AppState* a);
static void dlgc_refrescar(DlgCuadro* d);

struct DlgCarga {
    AppState* a = nullptr;
    AdwDialog* dialog = nullptr;
    AdwToastOverlay* toasts = nullptr;
    std::vector<std::vector<size_t>> rutas;
    std::vector<std::string> equipos;
    AdwComboRow *cuadro = nullptr, *tipo = nullptr, *fases = nullptr, *emer = nullptr, *zona = nullptr;
    std::vector<std::string> zonas;     // índice 0 del combo = heredada del cuadro
    std::vector<std::string> tipos;     // nombres de los tipos de carga del proyecto (orden del combo)
    AdwEntryRow *nombre = nullptr, *tag = nullptr;
    AdwSpinRow *cantidad = nullptr, *pNom = nullptr, *cos = nullptr, *eta = nullptr, *ku = nullptr, *ks = nullptr, *ke = nullptr;
    AdwSpinRow* longitud = nullptr;
    AdwSwitchRow *cosAuto = nullptr, *etaAuto = nullptr;
    // Modo edición: carga existente (cuadro de origen e índice); editIdx < 0 = nueva carga
    std::vector<size_t> rutaOrigen;
    int editIdx = -1;
    std::string tagOriginal;
    GtkWidget *rFases = nullptr, *rCos = nullptr, *rEta = nullptr, *rPabs = nullptr, *rPdem = nullptr,
              *rSdem = nullptr, *rSemer = nullptr, *rAvisos = nullptr;
    bool actualizando = false;
};

static void recoger_cuadros(const std::vector<Cuadro>& cs, std::vector<size_t> base, int nivel,
                            std::vector<std::vector<size_t>>& rutas, std::vector<std::string>& etiquetas) {
    for (size_t i = 0; i < cs.size(); i++) {
        std::vector<size_t> r = base; r.push_back(i);
        std::string sangria;
        for (int k = 0; k < nivel; k++) sangria += "    ";
        etiquetas.push_back(sangria + (nivel ? "↳ " : "") + cs[i].tag + " — " + cs[i].nombre);
        rutas.push_back(r);
        recoger_cuadros(cs[i].subcuadros, r, nivel + 1, rutas, etiquetas);
    }
}

static void recoger_tags(const std::vector<Cuadro>& cs, std::set<std::string>& tags) {
    for (const Cuadro& c: cs) {
        for (const Carga& x: c.cargas) tags.insert(x.tag);
        for (const Bateria& b: c.baterias) tags.insert(b.tag);
        recoger_tags(c.subcuadros, tags);
    }
}

static std::string tipo_sel(DlgCarga* d) {
    guint i = adw_combo_row_get_selected(d->tipo);
    return i < d->tipos.size() ? d->tipos[i] : "GENERAL";
}

static Carga dlg_leer(DlgCarga* d) {
    Carga c;
    c.tipo = tipo_sel(d);
    c.nombre = trim(gtk_editable_get_text(GTK_EDITABLE(d->nombre)));
    c.pNomKw = adw_spin_row_get_value(d->pNom);
    guint f = adw_combo_row_get_selected(d->fases);
    if (f > 0 && f <= configuracionesFases().size()) c.configFases = configuracionesFases()[f - 1];
    if (!adw_switch_row_get_active(d->cosAuto)) c.cosPhi = adw_spin_row_get_value(d->cos);
    if (d->a->proyecto.datos.usaRendimiento(c.tipo) && !adw_switch_row_get_active(d->etaAuto)) c.rendimientoPct = adw_spin_row_get_value(d->eta);
    c.ku = adw_spin_row_get_value(d->ku);
    c.ks = adw_spin_row_get_value(d->ks);
    c.ke = adw_spin_row_get_value(d->ke);
    guint e = adw_combo_row_get_selected(d->emer);
    if (e > 0 && e <= d->equipos.size()) c.tagEmer = d->equipos[e - 1];
    double l = adw_spin_row_get_value(d->longitud);
    if (l > 0) c.longitudM = l;
    guint z = adw_combo_row_get_selected(d->zona);
    if (z > 0 && z <= d->zonas.size()) c.zona = d->zonas[z - 1];
    return c;
}

static void set_resultado(GtkWidget* lbl, const std::string& s) { gtk_label_set_text(GTK_LABEL(lbl), s.c_str()); }

static void dlg_actualizar(DlgCarga* d) {
    if (d->actualizando) return;
    d->actualizando = true;

    std::string tipo = tipo_sel(d);
    bool conEta = d->a->proyecto.datos.usaRendimiento(tipo);
    gtk_widget_set_visible(GTK_WIDGET(d->etaAuto), conEta);
    gtk_widget_set_visible(GTK_WIDGET(d->eta), conEta);
    bool cosAuto = adw_switch_row_get_active(d->cosAuto);
    bool etaAuto = adw_switch_row_get_active(d->etaAuto);
    gtk_widget_set_sensitive(GTK_WIDGET(d->cos), !cosAuto);
    gtk_widget_set_sensitive(GTK_WIDGET(d->eta), !etaAuto);

    Carga c = dlg_leer(d);
    std::optional<std::string> fasesEquipo;
    if (c.tagEmer)
        for (const EquipoEmergencia& e: d->a->proyecto.equipos)
            if (e.tag == *c.tagEmer) fasesEquipo = e.fases;
    c.calcular(d->a->proyecto.datos, fasesEquipo);

    // Con «automático» activo, el campo muestra el valor que se usará en el cálculo
    if (cosAuto) adw_spin_row_set_value(d->cos, c.cosEfectivo);
    if (conEta && etaAuto) adw_spin_row_set_value(d->eta, c.etaPct);

    int n = int(adw_spin_row_get_value(d->cantidad));
    auto conTotal = [&](double v, const char* unidad) {
        std::string s = fmt(v) + " " + unidad;
        if (n > 1) s += "  (×" + std::to_string(n) + " = " + fmt(v * n) + " " + unidad + ")";
        return s;
    };
    set_resultado(d->rFases, c.fases + " / " + fmt(c.tensionV, 0) + " V");
    set_resultado(d->rCos, fmt(c.cosEfectivo, 3));
    set_resultado(d->rEta, conEta ? fmt(c.etaPct, 1) + " %" : "—");
    set_resultado(d->rPabs, conTotal(c.nKw, "kW"));
    set_resultado(d->rPdem, conTotal(c.pKw, "kW"));
    set_resultado(d->rSdem, conTotal(c.sKva, "kVA"));
    set_resultado(d->rSemer, conTotal(c.sEmerKva, "kVA"));
    set_resultado(d->rAvisos, c.avisos.empty() ? "Ninguno" : unir(c.avisos, "; "));

    d->actualizando = false;
}

static void dlg_on_notify(GObject*, GParamSpec*, gpointer data) { dlg_actualizar(static_cast<DlgCarga*>(data)); }
static void dlg_on_changed(GtkEditable*, gpointer data) { dlg_actualizar(static_cast<DlgCarga*>(data)); }

static void dlg_on_cancel(GtkButton*, gpointer data) { adw_dialog_close(static_cast<DlgCarga*>(data)->dialog); }

static void dlg_on_delete(GtkButton*, gpointer data) {
    DlgCarga* d = static_cast<DlgCarga*>(data);
    {
        AdwAlertDialog* al = ADW_ALERT_DIALOG(adw_alert_dialog_new(("¿Eliminar la carga " + d->tagOriginal + "?").c_str(), nullptr));
        adw_alert_dialog_add_response(al, "cancelar", "Cancelar");
        adw_alert_dialog_add_response(al, "eliminar", "Eliminar");
        adw_alert_dialog_set_response_appearance(al, "eliminar", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_close_response(al, "cancelar");
        g_signal_connect(al, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* resp, gpointer data) {
            if (g_strcmp0(resp, "eliminar") != 0) return;
            DlgCarga* d = static_cast<DlgCarga*>(data);
            Cuadro* c = d->a->proyecto.cuadroPorRuta(d->rutaOrigen);
            if (c && d->editIdx >= 0 && size_t(d->editIdx) < c->cargas.size()) {
                c->cargas.erase(c->cargas.begin() + d->editIdx);
                d->a->modificado = true;
                std::string msg = "Carga " + d->tagOriginal + " eliminada";
                AppState* a = d->a;
                adw_dialog_close(d->dialog);
                refresh_all(a);
                show_toast(a->toasts, msg);
            }
        }), d);
        adw_dialog_present(ADW_DIALOG(al), GTK_WIDGET(d->dialog));
    }
}

static void dlg_on_add(GtkButton*, gpointer data) {
    DlgCarga* d = static_cast<DlgCarga*>(data);
    AppState* a = d->a;

    guint ic = adw_combo_row_get_selected(d->cuadro);
    Cuadro* cu = ic < d->rutas.size() ? a->proyecto.cuadroPorRuta(d->rutas[ic]) : nullptr;
    if (!cu) { show_toast(d->toasts, "Selecciona el cuadro donde conectar la carga."); return; }

    Carga base = dlg_leer(d);
    if (base.pNomKw <= 0 && base.tipo != "RESERVA") {
        show_toast(d->toasts, "La potencia nominal debe ser mayor que 0 kW.");
        gtk_widget_grab_focus(GTK_WIDGET(d->pNom));
        return;
    }
    if (d->editIdx >= 0) {
        Cuadro* origen = a->proyecto.cuadroPorRuta(d->rutaOrigen);
        if (!origen || size_t(d->editIdx) >= origen->cargas.size()) { adw_dialog_close(d->dialog); return; }
        base.tag = d->tagOriginal;
        base.rev = origen->cargas[size_t(d->editIdx)].rev;   // la revisión se rellena en la lista de cargas
        if (!a->proyecto.datos.tagsAutomaticos) {
            std::string t = trim(gtk_editable_get_text(GTK_EDITABLE(d->tag)));
            if (t.empty()) { show_toast(d->toasts, "El TAG es obligatorio."); return; }
            std::set<std::string> usados;
            recoger_tags(a->proyecto.cuadros, usados);
            if (t != d->tagOriginal && usados.count(t)) { show_toast(d->toasts, "Ya existe una carga con el TAG «" + t + "»."); return; }
            base.tag = t;
        }
        if (d->rutas[ic] == d->rutaOrigen) {
            origen->cargas[size_t(d->editIdx)] = base;
        } else {   // se cambia de cuadro
            origen->cargas.erase(origen->cargas.begin() + d->editIdx);
            a->proyecto.cuadroPorRuta(d->rutas[ic])->cargas.push_back(base);
        }
        a->rutaSeleccionada = d->rutas[ic];
        a->modificado = true;
        refresh_all(a);
        adw_dialog_close(d->dialog);
        return;
    }

    int n = std::max(1, int(adw_spin_row_get_value(d->cantidad)));

    std::vector<std::string> tags(n);
    if (!a->proyecto.datos.tagsAutomaticos) {
        std::string t = trim(gtk_editable_get_text(GTK_EDITABLE(d->tag)));
        if (t.empty()) { show_toast(d->toasts, "El TAG es obligatorio (los TAG automáticos están desactivados)."); gtk_widget_grab_focus(GTK_WIDGET(d->tag)); return; }
        std::set<std::string> usados;
        recoger_tags(a->proyecto.cuadros, usados);
        for (int i = 0; i < n; i++) {
            tags[i] = n > 1 ? t + "-" + std::to_string(i + 1) : t;
            if (usados.count(tags[i])) { show_toast(d->toasts, "Ya existe una carga con el TAG «" + tags[i] + "»."); return; }
        }
    }

    for (int i = 0; i < n; i++) {
        Carga c = base;
        c.tag = tags[i];
        cu->cargas.push_back(c);
    }
    a->ultimoTipoCarga = base.tipo;
    a->rutaSeleccionada = d->rutas[ic];
    a->modificado = true;
    std::string msg = (n > 1 ? std::to_string(n) + " cargas añadidas a " : "Carga añadida a ") + cu->tag;
    refresh_all(a);
    show_toast(a->toasts, msg);
    adw_dialog_close(d->dialog);
}

static AdwSpinRow* spin_row(AdwPreferencesGroup* g, const char* titulo, const char* sub,
                            double min, double max, double paso, int digitos, double valor) {
    AdwSpinRow* r = ADW_SPIN_ROW(adw_spin_row_new_with_range(min, max, paso));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), titulo);
    if (sub) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), sub);
    adw_spin_row_set_digits(r, digitos);
    adw_spin_row_set_value(r, valor);
    adw_preferences_group_add(g, GTK_WIDGET(r));
    return r;
}

static AdwComboRow* combo_row(AdwPreferencesGroup* g, const char* titulo, const char* sub,
                              const std::vector<std::string>& items, guint sel) {
    GtkStringList* lista = gtk_string_list_new(nullptr);
    for (const std::string& s: items) gtk_string_list_append(lista, s.c_str());
    AdwComboRow* r = ADW_COMBO_ROW(adw_combo_row_new());
    adw_combo_row_set_model(r, G_LIST_MODEL(lista));
    g_object_unref(lista);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), titulo);
    if (sub) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), sub);
    if (sel < items.size()) adw_combo_row_set_selected(r, sel);
    adw_preferences_group_add(g, GTK_WIDGET(r));
    return r;
}

// Búsqueda en un desplegable de cadenas: necesita saber qué texto comparar (la cadena de cada elemento)
static void habilitar_busqueda(AdwComboRow* r) {
    adw_combo_row_set_expression(r, gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string"));
    adw_combo_row_set_enable_search(r, TRUE);
    adw_combo_row_set_search_match_mode(r, GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
}

static AdwEntryRow* entry_row(AdwPreferencesGroup* g, const char* titulo) {
    AdwEntryRow* r = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), titulo);
    adw_preferences_group_add(g, GTK_WIDGET(r));
    return r;
}

static AdwSwitchRow* switch_row(AdwPreferencesGroup* g, const char* titulo, const char* sub, bool activo) {
    AdwSwitchRow* r = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), titulo);
    if (sub) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), sub);
    adw_switch_row_set_active(r, activo);
    adw_preferences_group_add(g, GTK_WIDGET(r));
    return r;
}

static GtkWidget* resultado_row(AdwPreferencesGroup* g, const char* titulo, bool multilinea = false) {
    GtkWidget* r = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), titulo);
    GtkWidget* lbl = gtk_label_new("");
    gtk_widget_add_css_class(lbl, "numeric");
    gtk_label_set_xalign(GTK_LABEL(lbl), 1.0);
    if (multilinea) {
        gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
        gtk_label_set_width_chars(GTK_LABEL(lbl), 30);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 45);
    }
    adw_action_row_add_suffix(ADW_ACTION_ROW(r), lbl);
    adw_preferences_group_add(g, r);
    return lbl;
}

// editIdx >= 0: editar la carga nº editIdx del cuadro a->rutaSeleccionada
static void abrir_dialogo_carga(AppState* a, int editIdx = -1) {
    if (a->proyecto.cuadros.empty()) {
        show_error(a, "Añade primero un cuadro eléctrico donde conectar la carga.");
        return;
    }
    a->proyecto.recalcular();
    const Carga* edit = nullptr;
    if (editIdx >= 0) {
        const Cuadro* c = a->proyecto.cuadroPorRuta(a->rutaSeleccionada);
        if (!c || size_t(editIdx) >= c->cargas.size()) return;
        edit = &c->cargas[size_t(editIdx)];
    }

    DlgCarga* d = new DlgCarga;
    d->a = a;
    d->editIdx = editIdx;
    d->rutaOrigen = a->rutaSeleccionada;
    if (edit) d->tagOriginal = edit->tag;
    d->dialog = adw_dialog_new();
    g_object_set_data_full(G_OBJECT(d->dialog), "dlg", d, [](gpointer p) { delete static_cast<DlgCarga*>(p); });
    adw_dialog_set_title(d->dialog, edit ? ("Carga " + edit->tag).c_str() : "Nueva carga");
    adw_dialog_set_content_width(d->dialog, 600);
    adw_dialog_set_content_height(d->dialog, 760);

    AdwToolbarView* tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar* hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_start_title_buttons(hb, FALSE);
    adw_header_bar_set_show_end_title_buttons(hb, FALSE);
    GtkWidget* cancel = gtk_button_new_with_label("Cancelar");
    GtkWidget* add = gtk_button_new_with_label(edit ? "Aplicar" : "Añadir");
    gtk_widget_add_css_class(add, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(dlg_on_cancel), d);
    g_signal_connect(add, "clicked", G_CALLBACK(dlg_on_add), d);
    adw_header_bar_pack_start(hb, cancel);
    adw_header_bar_pack_end(hb, add);
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));

    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    const Proyecto& p = a->proyecto;

    // Ubicación e identificación
    AdwPreferencesGroup* gId = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gId, "Identificación");
    std::vector<std::string> etiquetas;
    recoger_cuadros(p.cuadros, {}, 0, d->rutas, etiquetas);
    guint selCuadro = 0;
    for (size_t i = 0; i < d->rutas.size(); i++) if (d->rutas[i] == a->rutaSeleccionada) selCuadro = guint(i);
    d->cuadro = combo_row(gId, "Cuadro de alimentación", "Cuadro o subcuadro al que se conecta la carga", etiquetas, selCuadro);
    habilitar_busqueda(d->cuadro);
    std::vector<std::string> zonas = {"Heredada del cuadro"};
    for (const Zona& z: p.datos.zonas) {
        d->zonas.push_back(z.tag);
        zonas.push_back(z.tag + (z.descripcion.empty() ? "" : " — " + z.descripcion));
    }
    d->zona = combo_row(gId, "Zona", "Primer campo del TAG: <zona>-<tipo>-<nº>", zonas, 0);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(d->zona), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(d->zona), !p.datos.zonas.empty());

    for (const TipoCargaDef& t: p.datos.tiposCarga) d->tipos.push_back(t.nombre);
    if (edit && !p.datos.tipoCarga(edit->tipo)) d->tipos.push_back(edit->tipo);   // tipo ya no definido
    const auto& tipos = d->tipos;
    std::vector<std::string> etiquetasTipo;
    for (const std::string& t: tipos) etiquetasTipo.push_back(p.datos.descripcionTipo(t));   // se elige por la descripción
    guint selTipo = guint(std::find(tipos.begin(), tipos.end(), a->ultimoTipoCarga) - tipos.begin());
    d->tipo = combo_row(gId, "Tipo de carga", "Se elige por su descripción", etiquetasTipo, selTipo < tipos.size() ? selTipo : 0);
    habilitar_busqueda(d->tipo);
    d->nombre = entry_row(gId, "Nombre / descripción");
    d->tag = entry_row(gId, p.datos.tagsAutomaticos ? "TAG (se asigna automáticamente)" : "TAG");
    gtk_widget_set_sensitive(GTK_WIDGET(d->tag), !p.datos.tagsAutomaticos);
    d->cantidad = spin_row(gId, "Cantidad", "Número de cargas iguales a añadir", 1, 500, 1, 0, 1);
    gtk_widget_set_visible(GTK_WIDGET(d->cantidad), edit == nullptr);
    adw_preferences_page_add(page, gId);

    // Datos eléctricos
    AdwPreferencesGroup* gEl = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gEl, "Datos eléctricos");
    d->pNom = spin_row(gEl, "Potencia activa nominal [kW]", nullptr, 0, 100000, 0.1, 2, 1.0);
    std::vector<std::string> fases = {"Automática (según equipo de emergencia)"};
    for (const std::string& f: configuracionesFases()) fases.push_back(f);
    d->fases = combo_row(gEl, "Configuración de fases", nullptr, fases, 0);
    d->cosAuto = switch_row(gEl, "cos φ automático", "Valor fijo del tipo de carga o tabla de motores", true);
    d->cos = spin_row(gEl, "cos φ", nullptr, 0.01, 1.0, 0.01, 3, 0.85);
    d->etaAuto = switch_row(gEl, "Rendimiento automático", "Según la tabla de motores", true);
    d->eta = spin_row(gEl, "Rendimiento η [%]", nullptr, 1, 100, 0.5, 1, 90);
    d->longitud = spin_row(gEl, "Longitud hasta el cuadro [m]", "Longitud de la línea desde el cuadro de alimentación (0 = sin definir)",
                           0, 10000, 1, 1, 0);
    adw_preferences_page_add(page, gEl);

    // Coeficientes
    AdwPreferencesGroup* gF = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gF, "Coeficientes");
    d->ku = spin_row(gF, "Factor de utilización Ku", nullptr, 0, 1, 0.05, 2, 1.0);
    d->ks = spin_row(gF, "Factor de simultaneidad Ks", nullptr, 0, 1, 0.05, 2, 1.0);
    d->ke = spin_row(gF, "Factor de emergencia Ke", "Fracción de la carga alimentada en emergencia", 0, 1, 0.05, 2, 0.0);
    std::vector<std::string> equipos = {"No aplica"};
    for (const EquipoEmergencia& e: p.equipos) {
        d->equipos.push_back(e.tag);
        equipos.push_back(etiqueta_equipo(e));
    }
    d->emer = combo_row(gF, "Equipo de emergencia", nullptr, equipos, 0);
    gtk_widget_set_sensitive(GTK_WIDGET(d->emer), !p.equipos.empty());
    adw_preferences_page_add(page, gF);

    // Vista previa del cálculo
    AdwPreferencesGroup* gR = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gR, "Resultado del cálculo");
    adw_preferences_group_set_description(gR, "Se actualiza al modificar los datos");
    d->rFases = resultado_row(gR, "Fases / tensión");
    d->rCos = resultado_row(gR, "cos φ efectivo");
    d->rEta = resultado_row(gR, "Rendimiento efectivo");
    d->rPabs = resultado_row(gR, "P absorbida");
    d->rPdem = resultado_row(gR, "P demandada");
    d->rSdem = resultado_row(gR, "S demandada");
    d->rSemer = resultado_row(gR, "S emergencia");
    d->rAvisos = resultado_row(gR, "Avisos", true);
    adw_preferences_page_add(page, gR);

    if (edit) {
        // Rellenar con los datos de la carga
        guint it = guint(std::find(tipos.begin(), tipos.end(), edit->tipo) - tipos.begin());
        if (it < tipos.size()) adw_combo_row_set_selected(d->tipo, it);
        gtk_editable_set_text(GTK_EDITABLE(d->nombre), edit->nombre.c_str());
        gtk_editable_set_text(GTK_EDITABLE(d->tag), edit->tag.c_str());
        adw_spin_row_set_value(d->pNom, edit->pNomKw);
        if (edit->configFases) {
            const auto& cf = configuracionesFases();
            guint k = guint(std::find(cf.begin(), cf.end(), *edit->configFases) - cf.begin());
            if (k < cf.size()) adw_combo_row_set_selected(d->fases, k + 1);
        }
        adw_switch_row_set_active(d->cosAuto, !edit->cosPhi);
        if (edit->cosPhi) adw_spin_row_set_value(d->cos, *edit->cosPhi);
        adw_switch_row_set_active(d->etaAuto, !edit->rendimientoPct);
        if (edit->rendimientoPct) adw_spin_row_set_value(d->eta, *edit->rendimientoPct);
        adw_spin_row_set_value(d->ku, edit->ku);
        adw_spin_row_set_value(d->ks, edit->ks);
        adw_spin_row_set_value(d->ke, edit->ke);
        for (size_t k = 0; k < d->equipos.size(); k++)
            if (edit->tagEmer && *edit->tagEmer == d->equipos[k]) adw_combo_row_set_selected(d->emer, guint(k + 1));
        if (edit->longitudM) adw_spin_row_set_value(d->longitud, *edit->longitudM);
        for (size_t k = 0; k < d->zonas.size(); k++)
            if (edit->zona == d->zonas[k]) adw_combo_row_set_selected(d->zona, guint(k + 1));

        // Eliminar la carga
        AdwPreferencesGroup* gDel = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        GtkWidget* bDel = gtk_button_new_with_label("Eliminar carga");
        gtk_widget_add_css_class(bDel, "destructive-action");
        gtk_widget_add_css_class(bDel, "pill");
        gtk_widget_set_halign(bDel, GTK_ALIGN_CENTER);
        g_signal_connect(bDel, "clicked", G_CALLBACK(dlg_on_delete), d);
        adw_preferences_group_add(gDel, bDel);
        adw_preferences_page_add(page, gDel);
    }

    d->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(d->toasts, GTK_WIDGET(page));
    adw_toolbar_view_set_content(tv, GTK_WIDGET(d->toasts));
    adw_dialog_set_child(d->dialog, GTK_WIDGET(tv));
    adw_dialog_set_default_widget(d->dialog, add);
    adw_dialog_set_focus(d->dialog, GTK_WIDGET(d->pNom));

    for (GObject* w: {G_OBJECT(d->cuadro), G_OBJECT(d->tipo), G_OBJECT(d->fases), G_OBJECT(d->emer)})
        g_signal_connect(w, "notify::selected", G_CALLBACK(dlg_on_notify), d);
    for (GObject* w: {G_OBJECT(d->cantidad), G_OBJECT(d->pNom), G_OBJECT(d->cos), G_OBJECT(d->eta),
                      G_OBJECT(d->ku), G_OBJECT(d->ks), G_OBJECT(d->ke)})
        g_signal_connect(w, "notify::value", G_CALLBACK(dlg_on_notify), d);
    for (GObject* w: {G_OBJECT(d->cosAuto), G_OBJECT(d->etaAuto)})
        g_signal_connect(w, "notify::active", G_CALLBACK(dlg_on_notify), d);
    g_signal_connect(d->nombre, "changed", G_CALLBACK(dlg_on_changed), d);

    dlg_actualizar(d);
    adw_dialog_present(d->dialog, a->window);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Exportación a Excel
// ─────────────────────────────────────────────────────────────────────────────

static void on_export_excel(GtkWidget*, gpointer data) {
    AppState* a = static_cast<AppState*>(data);
    if (a->proyecto.cuadros.empty()) { show_error(a, "El proyecto no tiene cuadros ni cargas que exportar."); return; }

    GtkFileDialog* d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Exportar lista de cargas a Excel");
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Libro de Excel (*.xlsx)");
    gtk_file_filter_add_suffix(filter, "xlsx");
    GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_object_unref(filter);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    g_object_unref(filters);

    std::string nombre = "lista_cargas.xlsx";
    if (!a->ruta.empty()) {
        std::filesystem::path pr(a->ruta);
        nombre = pr.stem().string() + ".xlsx";
        GFile* carpeta = g_file_new_for_path(pr.parent_path().c_str());
        gtk_file_dialog_set_initial_folder(d, carpeta);
        g_object_unref(carpeta);
    }
    gtk_file_dialog_set_initial_name(d, nombre.c_str());

    gtk_file_dialog_save(d, GTK_WINDOW(a->window), nullptr, [](GObject* src, GAsyncResult* r, gpointer user) {
        AppState* s = static_cast<AppState*>(user);
        GError* e = nullptr;
        GFile* f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), r, &e);
        if (!f) { if (e) g_error_free(e); return; }
        char* p = g_file_get_path(f);
        g_object_unref(f);
        if (!p) return;
        std::string ruta = p;
        g_free(p);
        std::string ext = std::filesystem::path(ruta).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".xlsx") ruta += ".xlsx";
        try {
            exportarExcel(s->proyecto, ruta);
            show_toast(s->toasts, "Lista de cargas exportada a " + std::filesystem::path(ruta).filename().string());
        } catch (const std::exception& x) {
            show_error(s, std::string("No se pudo exportar a Excel:\n") + x.what());
        }
    }, a);
    g_object_unref(d);
}

// ─────────────────────────────────────────────────────────────────────────────

static void update_title(AppState* a) {
    std::string name = a->ruta.empty() ? "(sin guardar)" : std::filesystem::path(a->ruta).filename().string();
    gtk_window_set_title(GTK_WINDOW(a->window), (std::string(a->modificado ? "*" : "") + name + " — Electrical Tools").c_str());
    if (a->status) {
        std::string status = a->ruta.empty() ? "Fichero nuevo, sin guardar" : a->ruta;
        if (a->modificado) status += " · modificado";
        gtk_label_set_text(GTK_LABEL(a->status), status.c_str());
    }
}

// Registra el estado actual del proyecto como paso deshacible (si ha cambiado)
static void registrar_cambio(AppState* a) {
    if (a->restaurando) return;
    std::string j = a->proyecto.toJson().dump();
    if (j == a->estadoActual) return;
    if (!a->estadoActual.empty()) {
        a->deshacer.push_back(a->estadoActual);
        if (a->deshacer.size() > 200) a->deshacer.erase(a->deshacer.begin());
    }
    a->rehacer.clear();
    a->estadoActual = j;
}

static void reiniciar_historial(AppState* a) {
    a->deshacer.clear();
    a->rehacer.clear();
    a->estadoActual.clear();
}

static void refresh_all(AppState* a) {
    recompute(a);
    refresh_general(a);
    refresh_cuadros(a);
    if (a->diagrama) a->diagrama->refrescar();
    if (a->dlgCuadro) dlgc_refrescar(a->dlgCuadro);
    update_title(a);
    registrar_cambio(a);
}

static void open_path(AppState* a, const std::string& ruta) {
    try { a->proyecto = abrirProyecto(ruta); a->ruta = ruta; a->modificado=false; a->rutaSeleccionada.clear(); reiniciar_historial(a); if(a->diagrama){a->diagrama->seleccionar({});a->diagrama->encuadrarAlMostrar();} refresh_all(a); }
    catch (const std::exception& e) { show_error(a, std::string("No se pudo abrir el fichero:\n")+e.what()); }
}

static void on_open(GtkWidget*, gpointer data) {
    AppState* a=(AppState*)data;
    GtkFileDialog* d=gtk_file_dialog_new();
    gtk_file_dialog_set_title(d,"Abrir lista de cargas");
    gtk_file_dialog_open(d,GTK_WINDOW(a->window),nullptr,[](GObject*,GAsyncResult*r,gpointer user){
        AppState* s=(AppState*)user;
        GError* e=nullptr; GFile* f=gtk_file_dialog_open_finish(GTK_FILE_DIALOG(g_async_result_get_source_object(r)),r,&e);
        if(f){char*p=g_file_get_path(f);if(p)open_path(s,p);g_free(p);g_object_unref(f);}else if(e){show_error(s,e->message);g_error_free(e);}
    },a);
}

static void on_save_as(GtkWidget*, gpointer data);

static void on_save(GtkWidget*, gpointer data) {
    AppState* a=(AppState*)data;
    if(a->ruta.empty()) { on_save_as(nullptr,data); return; }
    try{guardarProyecto(a->proyecto,a->ruta);a->modificado=false;update_title(a);}catch(const std::exception&e){show_error(a,std::string("No se pudo guardar:\n")+e.what());}
}

static void on_save_as(GtkWidget*, gpointer data) {
    AppState* a=(AppState*)data;
    GtkFileDialog* d=gtk_file_dialog_new(); gtk_file_dialog_set_title(d,"Guardar lista de cargas");
    GtkFileFilter* filter=gtk_file_filter_new();gtk_file_filter_set_name(filter,"Listas de cargas (*.list)");gtk_file_filter_add_suffix(filter,"list");
    GListStore* filters=g_list_store_new(GTK_TYPE_FILE_FILTER);g_list_store_append(filters,filter);
    gtk_file_dialog_set_filters(d,G_LIST_MODEL(filters));
    gtk_file_dialog_save(d,GTK_WINDOW(a->window),nullptr,[](GObject*,GAsyncResult*r,gpointer user){
        AppState* s=(AppState*)user;GError*e=nullptr;GFile*f=gtk_file_dialog_save_finish(GTK_FILE_DIALOG(g_async_result_get_source_object(r)),r,&e);
        if(f){char*p=g_file_get_path(f);if(p){try{guardarProyecto(s->proyecto,p);s->ruta=p;s->modificado=false;update_title(s);}catch(const std::exception&x){show_error(s,std::string("No se pudo guardar:\n")+x.what());}}g_free(p);g_object_unref(f);}else if(e){g_error_free(e);}
    },a);
}

static void on_new(GtkWidget*, gpointer data) {
    AppState* a=(AppState*)data;a->proyecto=Proyecto();a->ruta.clear();a->modificado=true;a->rutaSeleccionada.clear();reiniciar_historial(a);if(a->diagrama){a->diagrama->seleccionar({});a->diagrama->encuadrarAlMostrar();}refresh_all(a);
}

static void on_add_board(GtkWidget*, gpointer data) {
    AppState* a=(AppState*)data;
    Cuadro c; c.tipo="GENERAL BT"; c.tag=a->proyecto.tagLibreCuadro(c.tipo); c.nombre=tipoCuadro(c.tipo).descripcion;
    a->proyecto.cuadros.push_back(c);a->modificado=true;refresh_all(a);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Cuadros: edición y borrado (desde el diagrama de arquitectura)
// ─────────────────────────────────────────────────────────────────────────────

struct DlgCuadro {
    AppState* a = nullptr;
    AdwDialog* dialog = nullptr;
    AdwToastOverlay* toasts = nullptr;
    RutaCuadro ruta;
    AdwComboRow *tipo = nullptr, *fases = nullptr, *zona = nullptr, *trafo = nullptr, *emer = nullptr;
    AdwEntryRow *tag = nullptr, *nombre = nullptr;
    AdwSpinRow* ke = nullptr;
    AdwSwitchRow* conTrafo = nullptr;
    std::vector<std::string> zonas, trafos, equipos;
    std::string tipoAnterior;
    GtkWidget* listaCargas = nullptr;
    GtkWidget *rCargas = nullptr, *rSub = nullptr, *rPinst = nullptr, *rPQS = nullptr, *rCos = nullptr;
    // Batería de condensadores (sólo CGBT)
    AdwSwitchRow* batOn = nullptr;
    AdwSpinRow *batCos = nullptr, *batEscalon = nullptr;
    GtkWidget *rBatNec = nullptr, *rBatQ = nullptr, *rBatCos = nullptr;
};

static void dlgc_bateria_preview(DlgCuadro* d) {
    const Cuadro* c = d->a->proyecto.cuadroPorRuta(d->ruta);
    if (!c || !d->batOn) return;
    bool on = adw_switch_row_get_active(d->batOn);
    gtk_widget_set_sensitive(GTK_WIDGET(d->batCos), on);
    gtk_widget_set_sensitive(GTK_WIDGET(d->batEscalon), on);
    // Q del cuadro sin compensar
    double q = c->qKvar;
    for (const Bateria& b: c->baterias) q -= b.qKvar();
    Bateria b;
    b.cosObjetivo = adw_spin_row_get_value(d->batCos);
    b.escalonKvar = adw_spin_row_get_value(d->batEscalon);
    b.calcular(c->pKw, q);
    if (!on) {
        set_resultado(d->rBatNec, fmt(b.qNecesariaKvar) + " kVAr");
        set_resultado(d->rBatQ, "Sin batería");
        set_resultado(d->rBatCos, fmt(b.cosAntes, 3));
        return;
    }
    set_resultado(d->rBatNec, fmt(b.qNecesariaKvar) + " kVAr");
    set_resultado(d->rBatQ, b.qKvarNominal > 0 ? fmt(b.qKvarNominal, 1) + " kVAr" : "No necesaria");
    set_resultado(d->rBatCos, fmt(b.cosAntes, 3) + " → " + fmt(b.cosDespues, 3));
}

static void dlgc_refrescar(DlgCuadro* d) {
    const Cuadro* c = d->a->proyecto.cuadroPorRuta(d->ruta);
    if (!c || !d->listaCargas) return;
    vaciar_lista(d->listaCargas);
    for (size_t k = 0; k < c->cargas.size(); k++) {
        const Carga& x = c->cargas[k];
        std::string titulo = x.tag + (x.nombre.empty() ? "" : " — " + x.nombre);
        std::string sub = d->a->proyecto.datos.descripcionTipo(x.tipo) + " · " + x.fases + " / " + fmt(x.tensionV, 0) + " V · Pn " + fmt(x.pNomKw) + " kW";
        if (x.longitudM) sub += " · L " + fmt(*x.longitudM, 1) + " m";
        if (!x.zona.empty()) sub += " · zona " + x.zona;
        if (x.tagEmer) sub += " · emergencia " + *x.tagEmer;
        GtkWidget* fila = fila_info(titulo, sub, "P " + fmt(x.pKw) + " kW · S " + fmt(x.sKva) + " kVA", unir(x.avisos));
        GtkWidget* ir = gtk_image_new_from_icon_name("go-next-symbolic");
        adw_action_row_add_suffix(ADW_ACTION_ROW(fila), ir);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(fila), TRUE);
        gtk_widget_set_tooltip_text(fila, "Editar la carga");
        conectar_clic(fila, [d, k] { d->a->rutaSeleccionada = d->ruta; abrir_dialogo_carga(d->a, int(k)); }, "activated");
        gtk_list_box_append(GTK_LIST_BOX(d->listaCargas), fila);
    }
    for (const Bateria& b: c->baterias) {
        gtk_list_box_append(GTK_LIST_BOX(d->listaCargas),
            fila_info(b.tag + (b.nombre.empty() ? "" : " — " + b.nombre), d->a->proyecto.datos.descripcionTipo(TIPO_BATERIA) + " · " + b.configFases,
                      "Q " + fmt(b.qKvar()) + " kVAr", ""));
    }
    if (c->cargas.empty() && c->baterias.empty())
        gtk_list_box_append(GTK_LIST_BOX(d->listaCargas), fila_vacia("El cuadro no tiene cargas conectadas"));

    set_resultado(d->rCargas, std::to_string(c->cargas.size()));
    set_resultado(d->rSub, std::to_string(c->subcuadros.size()));
    set_resultado(d->rPinst, fmt(c->pInstKw) + " kW");
    set_resultado(d->rPQS, fmt(c->pKw) + " kW · " + fmt(c->qKvar) + " kVAr · " + fmt(c->sKva) + " kVA");
    set_resultado(d->rCos, fmt(c->cosPhi, 3));
    dlgc_bateria_preview(d);
}

static void dlgc_on_tipo(GObject*, GParamSpec*, gpointer data) {
    DlgCuadro* d = static_cast<DlgCuadro*>(data);
    guint i = adw_combo_row_get_selected(d->tipo);
    if (i >= tiposCuadro().size()) return;
    const TipoCuadro& nuevo = tiposCuadro()[i];
    // Si el nombre era la descripción por defecto del tipo anterior, se actualiza
    std::string nombre = trim(gtk_editable_get_text(GTK_EDITABLE(d->nombre)));
    if (nombre.empty() || nombre == tipoCuadro(d->tipoAnterior).descripcion)
        gtk_editable_set_text(GTK_EDITABLE(d->nombre), nuevo.descripcion.c_str());
    d->tipoAnterior = nuevo.tipo;
}

static void dlgc_on_save(GtkButton*, gpointer data) {
    DlgCuadro* d = static_cast<DlgCuadro*>(data);
    AppState* a = d->a;
    Cuadro* c = a->proyecto.cuadroPorRuta(d->ruta);
    if (!c) { adw_dialog_close(d->dialog); return; }

    std::string tag = trim(gtk_editable_get_text(GTK_EDITABLE(d->tag)));
    if (tag.empty()) { show_toast(d->toasts, "El TAG del cuadro es obligatorio."); gtk_widget_grab_focus(GTK_WIDGET(d->tag)); return; }
    bool repetido = false;
    std::function<void(const std::vector<Cuadro>&)> buscar = [&](const std::vector<Cuadro>& cs) {
        for (const Cuadro& x: cs) { if (&x != c && x.tag == tag) repetido = true; buscar(x.subcuadros); }
    };
    buscar(a->proyecto.cuadros);
    if (repetido) { show_toast(d->toasts, "Ya existe otro cuadro con el TAG «" + tag + "»."); return; }

    if (c->tag != tag) {   // mantener las referencias por TAG (SAI alimentados desde el cuadro, fotovoltaica)
        for (EquipoEmergencia& e: a->proyecto.equipos) if (e.cuadroAlimentacion == c->tag) e.cuadroAlimentacion = tag;
        for (InstalacionFV& f: a->proyecto.datos.fotovoltaica) if (f.cuadro == c->tag) f.cuadro = tag;
    }
    c->tag = tag;
    c->nombre = trim(gtk_editable_get_text(GTK_EDITABLE(d->nombre)));
    guint t = adw_combo_row_get_selected(d->tipo);
    if (t < tiposCuadro().size()) c->tipo = tiposCuadro()[t].tipo;
    guint f = adw_combo_row_get_selected(d->fases);
    if (f < configuracionesFases().size()) c->configFases = configuracionesFases()[f];
    guint z = adw_combo_row_get_selected(d->zona);
    c->zona = (z > 0 && z <= d->zonas.size()) ? d->zonas[z - 1] : "";
    if (d->trafo) {
        guint tr = adw_combo_row_get_selected(d->trafo);
        c->trafo = (tr > 0 && tr <= d->trafos.size()) ? d->trafos[tr - 1] : "";
        c->calcConTrafo = adw_switch_row_get_active(d->conTrafo);
    }
    c->ke = adw_spin_row_get_value(d->ke);
    guint e = adw_combo_row_get_selected(d->emer);
    if (e > 0 && e <= d->equipos.size()) c->tagEmer = d->equipos[e - 1]; else c->tagEmer.reset();
    if (!c->saiAlimentacion.empty() && (!c->tagEmer || *c->tagEmer != c->saiAlimentacion))
        c->saiAlimentacion.clear();   // otro equipo de emergencia: deja de colgar del SAI
    if (d->batOn) {
        c->compensacion = adw_switch_row_get_active(d->batOn);
        if (c->compensacion) {
            if (c->baterias.empty()) { Bateria b; b.nombre = "BATERÍA DE CONDENSADORES"; c->baterias.push_back(b); }
            c->baterias.resize(1);
            c->baterias[0].cosObjetivo = adw_spin_row_get_value(d->batCos);
            c->baterias[0].escalonKvar = adw_spin_row_get_value(d->batEscalon);
        } else {
            c->baterias.clear();
        }
    }

    a->modificado = true;
    refresh_all(a);
    adw_dialog_close(d->dialog);
}

static void abrir_dialogo_cuadro(AppState* a, const RutaCuadro& ruta) {
    Cuadro* c = a->proyecto.cuadroPorRuta(ruta);
    if (!c) return;
    const Proyecto& p = a->proyecto;
    bool origen = ruta.size() == 1;

    DlgCuadro* d = new DlgCuadro;
    d->a = a; d->ruta = ruta; d->tipoAnterior = c->tipo;
    d->dialog = adw_dialog_new();
    g_object_set_data_full(G_OBJECT(d->dialog), "dlg", d, [](gpointer x) {
        DlgCuadro* dc = static_cast<DlgCuadro*>(x);
        if (dc->a->dlgCuadro == dc) dc->a->dlgCuadro = nullptr;
        delete dc;
    });
    adw_dialog_set_title(d->dialog, ("Cuadro " + c->tag).c_str());
    adw_dialog_set_content_width(d->dialog, 620);
    adw_dialog_set_content_height(d->dialog, 760);

    AdwToolbarView* tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar* hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_start_title_buttons(hb, FALSE);
    adw_header_bar_set_show_end_title_buttons(hb, FALSE);
    GtkWidget* cancel = gtk_button_new_with_label("Cancelar");
    GtkWidget* save = gtk_button_new_with_label("Aplicar");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), d->dialog);
    g_signal_connect(save, "clicked", G_CALLBACK(dlgc_on_save), d);
    adw_header_bar_pack_start(hb, cancel);
    adw_header_bar_pack_end(hb, save);
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));

    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    AdwPreferencesGroup* gId = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gId, "Identificación");
    if (!c->saiAlimentacion.empty())
        adw_preferences_group_set_description(gId, ("Cuelga del SAI " + c->saiAlimentacion +
            " (su potencia se suma al cuadro que alimenta al SAI)").c_str());
    else if (origen) adw_preferences_group_set_description(gId, "Cuadro de origen: no está alimentado desde otro cuadro");
    else {
        const Cuadro* padre = a->proyecto.cuadroPorRuta(RutaCuadro(ruta.begin(), ruta.end() - 1));
        if (padre && c->saiAlimentacion.empty()) adw_preferences_group_set_description(gId, ("Alimentado desde " + padre->tag).c_str());
    }
    std::vector<std::string> tipos;
    guint selTipo = 0;
    for (size_t i = 0; i < tiposCuadro().size(); i++) {
        tipos.push_back(tiposCuadro()[i].tipo + " — " + tiposCuadro()[i].descripcion);
        if (tiposCuadro()[i].tipo == c->tipo) selTipo = guint(i);
    }
    d->tipo = combo_row(gId, "Tipo de cuadro", nullptr, tipos, selTipo);
    d->tag = entry_row(gId, "TAG");
    gtk_editable_set_text(GTK_EDITABLE(d->tag), c->tag.c_str());
    d->nombre = entry_row(gId, "Nombre");
    gtk_editable_set_text(GTK_EDITABLE(d->nombre), c->nombre.c_str());
    adw_preferences_page_add(page, gId);

    AdwPreferencesGroup* gEl = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gEl, "Datos eléctricos");
    const auto& fases = configuracionesFases();
    guint selF = guint(std::find(fases.begin(), fases.end(), c->configFases) - fases.begin());
    d->fases = combo_row(gEl, "Configuración de fases", nullptr, fases, selF < fases.size() ? selF : 2);

    std::vector<std::string> zonas = {origen ? "Sin zona" : "Heredada del cuadro de alimentación"};
    guint selZ = 0;
    for (const Zona& z: p.datos.zonas) {
        d->zonas.push_back(z.tag);
        zonas.push_back(z.tag + (z.descripcion.empty() ? "" : " — " + z.descripcion));
        if (z.tag == c->zona) selZ = guint(d->zonas.size());
    }
    d->zona = combo_row(gEl, "Zona", "Se usa para los TAG automáticos de las cargas", zonas, selZ);

    if (origen) {
        std::vector<std::string> trafos = {"Ninguno"};
        guint selT = 0;
        for (const Transformador& t: p.datos.transformadores) {
            d->trafos.push_back(t.tag);
            trafos.push_back(t.tag + " — " + fmt(t.sNomKva, 0) + " kVA / " + fmt(t.uSecV, 0) + " V");
            if (t.tag == c->trafo) selT = guint(d->trafos.size());
        }
        d->trafo = combo_row(gEl, "Transformador de alimentación", "Fija la tensión del CGBT y de sus subcuadros", trafos, selT);
        d->conTrafo = switch_row(gEl, "Dimensionar con la S admisible del transformador", nullptr, c->calcConTrafo);
        gtk_widget_set_sensitive(GTK_WIDGET(d->trafo), !d->trafos.empty());
    }
    d->ke = spin_row(gEl, "Factor de emergencia Ke", nullptr, 0, 1, 0.05, 2, c->ke);
    std::vector<std::string> equipos = {"No aplica"};
    guint selE = 0;
    for (const EquipoEmergencia& e: p.equipos) {
        d->equipos.push_back(e.tag);
        equipos.push_back(etiqueta_equipo(e));
        if (c->tagEmer && *c->tagEmer == e.tag) selE = guint(d->equipos.size());
    }
    d->emer = combo_row(gEl, "Equipo de emergencia", nullptr, equipos, selE);
    gtk_widget_set_sensitive(GTK_WIDGET(d->emer), !p.equipos.empty());
    adw_preferences_page_add(page, gEl);

    AdwPreferencesGroup* gR = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gR, "Resultado");
    d->rCargas = resultado_row(gR, "Cargas conectadas");
    d->rSub = resultado_row(gR, "Subcuadros");
    d->rPinst = resultado_row(gR, "P instalada");
    d->rPQS = resultado_row(gR, "P / Q / S demandadas");
    d->rCos = resultado_row(gR, "cos φ");
    adw_preferences_page_add(page, gR);

    AdwPreferencesGroup* gC = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gC, "Cargas");
    GtkWidget* bCarga = gtk_button_new();
    GtkWidget* bc = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(bc), "list-add-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(bc), "Añadir carga…");
    gtk_button_set_child(GTK_BUTTON(bCarga), bc);
    gtk_widget_add_css_class(bCarga, "flat");
    conectar_clic(bCarga, [d] { d->a->rutaSeleccionada = d->ruta; abrir_dialogo_carga(d->a); });
    adw_preferences_group_set_header_suffix(gC, bCarga);
    d->listaCargas = nueva_lista();
    adw_preferences_group_add(gC, d->listaCargas);
    adw_preferences_page_add(page, gC);

    if (origen) {
        AdwPreferencesGroup* gB = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(gB, "Batería de condensadores");
        adw_preferences_group_set_description(gB,
            "Automática en cada CGBT: se dimensiona con la P y la Q demandadas totales del cuadro (incluidos sus "
            "subcuadros) para alcanzar el cos φ objetivo, redondeando al escalón de regulación");
        const Bateria* b = c->baterias.empty() ? nullptr : &c->baterias.front();
        d->batOn = switch_row(gB, "Con batería de condensadores", "También se añade y quita desde «Equipo» en la arquitectura", c->compensacion);
        d->batCos = spin_row(gB, "cos φ objetivo", nullptr, 0.80, 1.0, 0.01, 2, b ? b->cosObjetivo : 0.95);
        d->batEscalon = spin_row(gB, "Escalón de regulación [kVAr]", nullptr, 0.5, 500, 2.5, 1, b ? b->escalonKvar : 25);
        d->rBatNec = resultado_row(gB, "Q necesaria");
        d->rBatQ = resultado_row(gB, "Potencia de la batería");
        d->rBatCos = resultado_row(gB, "cos φ sin → con batería");
        adw_preferences_page_add(page, gB);
        for (GObject* w: {G_OBJECT(d->batCos), G_OBJECT(d->batEscalon)})
            g_signal_connect_swapped(w, "notify::value", G_CALLBACK(+[](DlgCuadro* d) { dlgc_bateria_preview(d); }), d);
        g_signal_connect_swapped(d->batOn, "notify::active", G_CALLBACK(+[](DlgCuadro* d) { dlgc_bateria_preview(d); }), d);
    }
    a->dlgCuadro = d;
    dlgc_refrescar(d);

    g_signal_connect(d->tipo, "notify::selected", G_CALLBACK(dlgc_on_tipo), d);

    d->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(d->toasts, GTK_WIDGET(page));
    adw_toolbar_view_set_content(tv, GTK_WIDGET(d->toasts));
    adw_dialog_set_child(d->dialog, GTK_WIDGET(tv));
    adw_dialog_set_default_widget(d->dialog, save);
    adw_dialog_present(d->dialog, a->window);
}

static void on_eliminar_cuadro_resp(AdwAlertDialog* dlg, const char* resp, gpointer data) {
    AppState* a = static_cast<AppState*>(data);
    if (g_strcmp0(resp, "eliminar") != 0) return;
    RutaCuadro r = *static_cast<RutaCuadro*>(g_object_get_data(G_OBJECT(dlg), "ruta"));
    if (r.empty() || !a->proyecto.cuadroPorRuta(r)) return;
    std::vector<Cuadro>& v = r.size() == 1 ? a->proyecto.cuadros
                                           : a->proyecto.cuadroPorRuta(RutaCuadro(r.begin(), r.end() - 1))->subcuadros;
    std::string tag = v[r.back()].tag;
    v.erase(v.begin() + long(r.back()));
    a->rutaSeleccionada.clear();
    if (a->diagrama) a->diagrama->seleccionar({});
    a->modificado = true;
    refresh_all(a);
    show_toast(a->toasts, "Cuadro " + tag + " eliminado");
}

static void confirmar_eliminar_cuadro(AppState* a, const RutaCuadro& r) {
    const Cuadro* c = a->proyecto.cuadroPorRuta(r);
    if (!c) return;
    size_t nSub = 0, nCargas = c->cargas.size();
    std::function<void(const std::vector<Cuadro>&)> contar = [&](const std::vector<Cuadro>& cs) {
        for (const Cuadro& x: cs) { nSub++; nCargas += x.cargas.size(); contar(x.subcuadros); }
    };
    contar(c->subcuadros);
    std::string cuerpo = (nSub == 0 && nCargas == 0)
        ? "El cuadro no tiene cargas ni subcuadros."
        : "Se eliminarán también " + std::to_string(nSub) + " subcuadro(s) y " + std::to_string(nCargas) + " carga(s) que cuelgan de él.";
    AdwAlertDialog* d = ADW_ALERT_DIALOG(adw_alert_dialog_new(("¿Eliminar el cuadro " + c->tag + "?").c_str(), cuerpo.c_str()));
    adw_alert_dialog_add_response(d, "cancelar", "Cancelar");
    adw_alert_dialog_add_response(d, "eliminar", "Eliminar");
    adw_alert_dialog_set_response_appearance(d, "eliminar", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(d, "cancelar");
    adw_alert_dialog_set_close_response(d, "cancelar");
    g_object_set_data_full(G_OBJECT(d), "ruta", new RutaCuadro(r), [](gpointer x) { delete static_cast<RutaCuadro*>(x); });
    g_signal_connect(d, "response", G_CALLBACK(on_eliminar_cuadro_resp), a);
    adw_dialog_present(ADW_DIALOG(d), a->window);
}

static void dialogo_trafo(AppState* a, int idx);
static void dialogo_ge(AppState* a, int idx);
static void dialogo_fv(AppState* a, int idx);
static void borrar_trafo(AppState* a, size_t idx);
static void borrar_ge(AppState* a, size_t idx);
static void borrar_fv(AppState* a, size_t idx);
static void dialogo_sai(AppState* a, int idx);
static void dialogo_bateria(AppState* a, int idxCgbt);
static void borrar_bateria(AppState* a, size_t idxCgbt);

static CallbacksDiagrama callbacks_diagrama(AppState* a) {
    CallbacksDiagrama cb;
    cb.cambioEstructura = [a] { a->modificado = true; refresh_all(a); };
    cb.cambioPosicion = [a] { a->modificado = true; update_title(a); registrar_cambio(a); };
    cb.editar = [a](const RutaCuadro& r) { abrir_dialogo_cuadro(a, r); };
    cb.eliminar = [a](const RutaCuadro& r) { confirmar_eliminar_cuadro(a, r); };
    cb.seleccion = [a](const RutaCuadro& r) { if (!r.empty()) a->rutaSeleccionada = r; };
    cb.aviso = [a](const std::string& m) { show_toast(a->toasts, m); };
    cb.editarFuente = [a](TipoNodo t, size_t i) {
        if (t == TipoNodo::Trafo) dialogo_trafo(a, int(i));
        else if (t == TipoNodo::Grupo) dialogo_ge(a, int(i));
        else if (t == TipoNodo::SAI) dialogo_sai(a, int(i));
        else if (t == TipoNodo::Bateria) dialogo_bateria(a, int(i));
        else if (t == TipoNodo::FV) dialogo_fv(a, int(i));
    };
    cb.eliminarFuente = [a](TipoNodo t, size_t i) {
        if (t == TipoNodo::Trafo) borrar_trafo(a, i);
        else if (t == TipoNodo::Grupo || t == TipoNodo::SAI) borrar_ge(a, i);
        else if (t == TipoNodo::Bateria) borrar_bateria(a, i);
        else if (t == TipoNodo::FV) borrar_fv(a, i);
    };
    cb.nuevaFuente = [a](TipoNodo t) {
        if (t == TipoNodo::Trafo) dialogo_trafo(a, -1);
        else if (t == TipoNodo::Grupo) dialogo_ge(a, -1);
        else if (t == TipoNodo::SAI) dialogo_sai(a, -1);
        else if (t == TipoNodo::Bateria) dialogo_bateria(a, -1);
        else if (t == TipoNodo::FV) dialogo_fv(a, -1);
    };
    return cb;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Pestaña «General»: transformadores, grupos electrógenos y fotovoltaica
// ─────────────────────────────────────────────────────────────────────────────

static const char* TIPO_GE = "G. ELEC.";

// Diálogo genérico de edición: una página con un grupo de filas y un botón «Aplicar»
struct DlgSimple {
    AppState* a = nullptr;
    AdwDialog* dialog = nullptr;
    AdwToastOverlay* toasts = nullptr;
    AdwPreferencesPage* page = nullptr;
    AdwPreferencesGroup* grupo = nullptr;
    std::function<bool(DlgSimple*)> aplicar;   // devuelve false si hay errores (ya notificados)
};

static DlgSimple* dlg_simple(AppState* a, const std::string& titulo, const char* boton, int alto) {
    DlgSimple* d = new DlgSimple;
    d->a = a;
    d->dialog = adw_dialog_new();
    g_object_set_data_full(G_OBJECT(d->dialog), "dlg", d, [](gpointer x) { delete static_cast<DlgSimple*>(x); });
    adw_dialog_set_title(d->dialog, titulo.c_str());
    adw_dialog_set_content_width(d->dialog, 540);
    adw_dialog_set_content_height(d->dialog, alto);

    AdwToolbarView* tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar* hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_start_title_buttons(hb, FALSE);
    adw_header_bar_set_show_end_title_buttons(hb, FALSE);
    GtkWidget* cancel = gtk_button_new_with_label("Cancelar");
    GtkWidget* ok = gtk_button_new_with_label(boton);
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), d->dialog);
    conectar_clic(ok, [d] {
        if (d->aplicar && !d->aplicar(d)) return;
        d->a->modificado = true;
        refresh_all(d->a);
        adw_dialog_close(d->dialog);
    });
    adw_header_bar_pack_start(hb, cancel);
    adw_header_bar_pack_end(hb, ok);
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));

    d->page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    d->grupo = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_page_add(d->page, d->grupo);
    d->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(d->toasts, GTK_WIDGET(d->page));
    adw_toolbar_view_set_content(tv, GTK_WIDGET(d->toasts));
    adw_dialog_set_child(d->dialog, GTK_WIDGET(tv));
    adw_dialog_set_default_widget(d->dialog, ok);
    return d;
}

static void set_entry(AdwEntryRow* r, const std::string& v) { gtk_editable_set_text(GTK_EDITABLE(r), v.c_str()); }
static std::string get_entry(AdwEntryRow* r) { return trim(gtk_editable_get_text(GTK_EDITABLE(r))); }

static void confirmar(AppState* a, const std::string& titulo, const std::string& cuerpo, std::function<void()> fn) {
    AdwAlertDialog* d = ADW_ALERT_DIALOG(adw_alert_dialog_new(titulo.c_str(), cuerpo.c_str()));
    adw_alert_dialog_add_response(d, "cancelar", "Cancelar");
    adw_alert_dialog_add_response(d, "eliminar", "Eliminar");
    adw_alert_dialog_set_response_appearance(d, "eliminar", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(d, "cancelar");
    adw_alert_dialog_set_close_response(d, "cancelar");
    g_signal_connect_data(d, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char* resp, gpointer f) {
            if (g_strcmp0(resp, "eliminar") == 0) (*static_cast<std::function<void()>*>(f))();
        }),
        new std::function<void()>(std::move(fn)),
        [](gpointer f, GClosure*) { delete static_cast<std::function<void()>*>(f); }, GConnectFlags(0));
    adw_dialog_present(ADW_DIALOG(d), a->window);
}

// Fila con botones de editar y eliminar
static GtkWidget* fila_editable(const std::string& titulo, const std::string& subtitulo, const std::string& avisos,
                                std::function<void()> editar, std::function<void()> borrar) {
    GtkWidget* r = fila_info(titulo, subtitulo, "", avisos);
    GtkWidget* be = gtk_button_new_from_icon_name("document-edit-symbolic");
    GtkWidget* bb = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_set_tooltip_text(be, "Editar");
    gtk_widget_set_tooltip_text(bb, "Eliminar");
    for (GtkWidget* b: {be, bb}) { gtk_widget_add_css_class(b, "flat"); gtk_widget_set_valign(b, GTK_ALIGN_CENTER); }
    conectar_clic(be, editar);
    conectar_clic(bb, std::move(borrar));
    adw_action_row_add_suffix(ADW_ACTION_ROW(r), be);
    adw_action_row_add_suffix(ADW_ACTION_ROW(r), bb);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(r), be);
    return r;
}

static void recorrer_cuadros(std::vector<Cuadro>& cs, const std::function<void(Cuadro&)>& fn) {
    for (Cuadro& c: cs) { fn(c); recorrer_cuadros(c.subcuadros, fn); }
}

// ── Transformadores ─────────────────────────────────────────────────────────

static void dialogo_trafo(AppState* a, int idx) {
    bool nuevo = idx < 0;
    Transformador t;
    if (!nuevo) t = a->proyecto.datos.transformadores[size_t(idx)];
    else {
        std::set<std::string> usados;
        for (const Transformador& x: a->proyecto.datos.transformadores) usados.insert(x.tag);
        int n = 1;
        while (usados.count("TR" + std::to_string(n))) n++;
        t.tag = "TR" + std::to_string(n); t.sNomKva = 1000; t.sAdmKva = 1000; t.uSecV = 400;
    }
    DlgSimple* d = dlg_simple(a, nuevo ? "Nuevo transformador" : "Transformador " + t.tag, nuevo ? "Añadir" : "Aplicar", 560);

    // 1) Datos de UN transformador
    adw_preferences_group_set_title(d->grupo, "Transformador");
    adw_preferences_group_set_description(d->grupo, "Datos de una unidad");
    AdwEntryRow* tag = entry_row(d->grupo, "TAG");
    set_entry(tag, t.tag);
    AdwSpinRow* sNom = spin_row(d->grupo, "Potencia nominal [kVA]", nullptr, 1, 100000, 50, 0, t.sNomKva);
    AdwSpinRow* uSec = spin_row(d->grupo, "Tensión secundaria [V]", nullptr, 100, 1000, 10, 0, t.uSecV);
    AdwSpinRow* sAdm = spin_row(d->grupo, "Potencia admisible [kVA]",
                                "Límite para el índice de carga y el dimensionado (normalmente igual a la nominal)",
                                1, 100000, 50, 0, t.sAdmKva);

    // 2) Número de transformadores iguales en paralelo
    AdwPreferencesGroup* gPar = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gPar, "Transformadores en paralelo");
    adw_preferences_group_set_description(gPar, "Todos de la misma potencia y tensión");
    AdwSpinRow* nPar = spin_row(gPar, "Número de transformadores", nullptr, 1, 20, 1, 0, t.nParalelo);
    GtkWidget* rTot = resultado_row(gPar, "Potencia total nominal");
    GtkWidget* rAdm = resultado_row(gPar, "Potencia total admisible");
    adw_preferences_page_add(d->page, gPar);

    auto total = [nPar, sNom, sAdm, uSec, rTot, rAdm] {
        double n = adw_spin_row_get_value(nPar);
        std::string pre = n > 1 ? fmt(n, 0) + " × " + fmt(adw_spin_row_get_value(sNom), 0) + " kVA = " : "";
        set_resultado(rTot, pre + fmt(n * adw_spin_row_get_value(sNom), 0) + " kVA · " + fmt(adw_spin_row_get_value(uSec), 0) + " V");
        set_resultado(rAdm, fmt(n * adw_spin_row_get_value(sAdm), 0) + " kVA");
    };
    total();
    for (AdwSpinRow* r: {nPar, sNom, sAdm, uSec}) conectar_notify(r, "notify::value", total);
    // La admisible acompaña a la nominal mientras el usuario no la haya cambiado
    auto nomAnterior = std::make_shared<double>(t.sNomKva);
    conectar_notify(sNom, "notify::value", [sNom, sAdm, nomAnterior] {
        double v = adw_spin_row_get_value(sNom);
        if (std::abs(adw_spin_row_get_value(sAdm) - *nomAnterior) < 1e-6) adw_spin_row_set_value(sAdm, v);
        *nomAnterior = v;
    });
    d->aplicar = [a, idx, tag, nPar, sNom, sAdm, uSec, viejo = t.tag](DlgSimple* d) {
        std::string nt = get_entry(tag);
        auto& lista = a->proyecto.datos.transformadores;
        if (nt.empty()) { show_toast(d->toasts, "El TAG es obligatorio."); return false; }
        for (size_t i = 0; i < lista.size(); i++)
            if (int(i) != idx && lista[i].tag == nt) { show_toast(d->toasts, "Ya existe un transformador «" + nt + "»."); return false; }
        Transformador x = idx < 0 ? Transformador() : lista[size_t(idx)];   // conserva la posición en el diagrama
        x.tag = nt; x.sNomKva = adw_spin_row_get_value(sNom); x.sAdmKva = adw_spin_row_get_value(sAdm);
        x.uSecV = adw_spin_row_get_value(uSec); x.nParalelo = int(adw_spin_row_get_value(nPar));
        if (idx < 0) lista.push_back(x);
        else {
            lista[size_t(idx)] = x;
            if (viejo != nt) recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) { if (c.trafo == viejo) c.trafo = nt; });
        }
        return true;
    };
    adw_dialog_present(d->dialog, a->window);
}

static void borrar_trafo(AppState* a, size_t idx) {
    std::string tag = a->proyecto.datos.transformadores[idx].tag;
    int usos = 0;
    recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) { if (c.trafo == tag) usos++; });
    std::string cuerpo = usos ? std::to_string(usos) + " cuadro(s) se alimentan de él y quedarán sin transformador asignado."
                              : "Ningún cuadro se alimenta de este transformador.";
    confirmar(a, "¿Eliminar el transformador " + tag + "?", cuerpo, [a, tag] {
        auto& l = a->proyecto.datos.transformadores;
        l.erase(std::remove_if(l.begin(), l.end(), [&](const Transformador& t) { return t.tag == tag; }), l.end());
        recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) { if (c.trafo == tag) c.trafo.clear(); });
        a->modificado = true;
        refresh_all(a);
    });
}

// ── Grupos electrógenos ─────────────────────────────────────────────────────

static void dialogo_ge(AppState* a, int idx) {
    bool nuevo = idx < 0;
    EquipoEmergencia e;
    e.tipo = TIPO_GE; e.horas = 8;
    if (!nuevo) e = a->proyecto.equipos[size_t(idx)];
    else {
        std::set<std::string> usados;
        for (const EquipoEmergencia& x: a->proyecto.equipos) usados.insert(x.tag);
        int n = 1;
        while (usados.count("GE" + std::to_string(n))) n++;
        e.tag = "GE" + std::to_string(n); e.nombre = "GRUPO ELECTRÓGENO " + std::to_string(n);
    }
    DlgSimple* d = dlg_simple(a, nuevo ? "Nuevo grupo electrógeno" : "Grupo electrógeno " + e.tag, nuevo ? "Añadir" : "Aplicar", 560);
    adw_preferences_group_set_description(d->grupo,
        "La potencia se calcula a partir de las cargas y cuadros asociados a este equipo (factor Ke).");
    AdwEntryRow* tag = entry_row(d->grupo, "TAG");
    set_entry(tag, e.tag);
    AdwEntryRow* nombre = entry_row(d->grupo, "Nombre");
    set_entry(nombre, e.nombre);
    AdwSpinRow* horas = spin_row(d->grupo, "Autonomía [h]", nullptr, 0, 1000, 0.5, 1, e.horas);
    AdwSpinRow* consumo = spin_row(d->grupo, "Consumo de combustible [L/h]", nullptr, 0, 100000, 1, 1, e.consumoLH);
    AdwSpinRow* deposito = spin_row(d->grupo, "Depósito incluido en el grupo [L]", nullptr, 0, 1000000, 10, 0, e.vDepositoInclL);
    AdwSwitchRow* tri = switch_row(d->grupo, "Forzar trifásico", "Aunque la potencia sea inferior al umbral 1F/3F", e.forzarTrifasico);
    d->aplicar = [a, idx, tag, nombre, horas, consumo, deposito, tri, viejo = e.tag](DlgSimple* d) {
        std::string nt = get_entry(tag);
        auto& lista = a->proyecto.equipos;
        if (nt.empty()) { show_toast(d->toasts, "El TAG es obligatorio."); return false; }
        for (size_t i = 0; i < lista.size(); i++)
            if (int(i) != idx && lista[i].tag == nt) { show_toast(d->toasts, "Ya existe un equipo de emergencia «" + nt + "»."); return false; }
        EquipoEmergencia x = idx < 0 ? EquipoEmergencia() : lista[size_t(idx)];
        x.tipo = TIPO_GE; x.tag = nt; x.nombre = get_entry(nombre);
        x.horas = adw_spin_row_get_value(horas); x.consumoLH = adw_spin_row_get_value(consumo);
        x.vDepositoInclL = adw_spin_row_get_value(deposito); x.forzarTrifasico = adw_switch_row_get_active(tri);
        if (idx < 0) lista.push_back(x);
        else {
            lista[size_t(idx)] = x;
            if (viejo != nt) recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) {
                if (c.tagEmer && *c.tagEmer == viejo) c.tagEmer = nt;
                if (c.saiAlimentacion == viejo) c.saiAlimentacion = nt;
                for (Carga& k: c.cargas) if (k.tagEmer && *k.tagEmer == viejo) k.tagEmer = nt;
            });
        }
        return true;
    };
    adw_dialog_present(d->dialog, a->window);
}

static void borrar_ge(AppState* a, size_t idx) {
    const EquipoEmergencia& e = a->proyecto.equipos[idx];
    std::string tag = e.tag;
    std::string cuerpo = e.nConsumidores ? std::to_string(e.nConsumidores) + " carga(s) o cuadro(s) asociados quedarán sin equipo de emergencia."
                                         : "No tiene cargas ni cuadros asociados.";
    std::string que = e.tipo == "UPS" ? "el SAI " : "el grupo electrógeno ";
    confirmar(a, "¿Eliminar " + que + tag + "?", cuerpo, [a, tag] {
        auto& l = a->proyecto.equipos;
        l.erase(std::remove_if(l.begin(), l.end(), [&](const EquipoEmergencia& x) { return x.tag == tag; }), l.end());
        a->proyecto.desasociarEmergencia(tag);
        a->modificado = true;
        refresh_all(a);
    });
}

// ── SAI ─────────────────────────────────────────────────────────────────────

static const char* TIPO_SAI = "UPS";

static void dialogo_sai(AppState* a, int idx) {
    bool nuevo = idx < 0;
    EquipoEmergencia e;
    e.tipo = TIPO_SAI; e.horas = 0.25; e.eficienciaPct = 95;
    if (!nuevo) e = a->proyecto.equipos[size_t(idx)];
    else {
        std::set<std::string> usados;
        for (const EquipoEmergencia& x: a->proyecto.equipos) usados.insert(x.tag);
        int n = 1;
        while (usados.count("SAI" + std::to_string(n))) n++;
        e.tag = "SAI" + std::to_string(n); e.nombre = "SISTEMA DE ALIMENTACIÓN ININTERRUMPIDA " + std::to_string(n);
    }
    DlgSimple* d = dlg_simple(a, nuevo ? "Nuevo SAI" : "SAI " + e.tag, nuevo ? "Añadir" : "Aplicar", 560);
    adw_preferences_group_set_description(d->grupo,
        "La potencia se calcula a partir de las cargas y cuadros asociados a este SAI (factor Ke).");
    AdwEntryRow* tag = entry_row(d->grupo, "TAG");
    set_entry(tag, e.tag);
    AdwEntryRow* nombre = entry_row(d->grupo, "Nombre");
    set_entry(nombre, e.nombre);
    AdwSpinRow* horas = spin_row(d->grupo, "Autonomía [h]", "Tiempo de funcionamiento con baterías", 0, 100, 0.25, 2, e.horas);
    AdwSpinRow* efic = spin_row(d->grupo, "Rendimiento [%]", "Para calcular la capacidad de las baterías", 50, 100, 0.5, 1, e.eficienciaPct);
    AdwSwitchRow* tri = switch_row(d->grupo, "Forzar trifásico", "Aunque la potencia sea inferior al umbral 1F/3F", e.forzarTrifasico);
    std::vector<std::string> tagsCuadro = {""}, etiquetas = {"Sin definir"};
    {
        std::vector<std::vector<size_t>> rutas;
        std::vector<std::string> lbls;
        recoger_cuadros(a->proyecto.cuadros, {}, 0, rutas, lbls);
        for (size_t i = 0; i < rutas.size(); i++) { tagsCuadro.push_back(a->proyecto.cuadroPorRuta(rutas[i])->tag); etiquetas.push_back(lbls[i]); }
    }
    guint selC = guint(std::find(tagsCuadro.begin(), tagsCuadro.end(), e.cuadroAlimentacion) - tagsCuadro.begin());
    AdwComboRow* alim = combo_row(d->grupo, "Cuadro de alimentación", "Cuadro del que se alimenta el SAI",
                                  etiquetas, selC < tagsCuadro.size() ? selC : 0);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(alim), FALSE);
    if (!nuevo) {
        AdwPreferencesGroup* gR = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(gR, "Resultado");
        set_resultado(resultado_row(gR, "S de emergencia"), fmt(e.sEmerKva) + " kVA (" + std::to_string(e.nConsumidores) + " consumidor(es))");
        set_resultado(resultado_row(gR, "SAI normalizado"), e.sDisenoKva ? fmt(*e.sDisenoKva, 1) + " kVA · " + e.fases : "—");
        set_resultado(resultado_row(gR, "Capacidad de baterías"), e.capBateriaKwh ? fmt(*e.capBateriaKwh) + " kWh" : "—");
        adw_preferences_page_add(d->page, gR);
    }
    d->aplicar = [a, idx, tag, nombre, horas, efic, tri, alim, tagsCuadro, viejo = e.tag](DlgSimple* d) {
        std::string nt = get_entry(tag);
        auto& lista = a->proyecto.equipos;
        if (nt.empty()) { show_toast(d->toasts, "El TAG es obligatorio."); return false; }
        for (size_t i = 0; i < lista.size(); i++)
            if (int(i) != idx && lista[i].tag == nt) { show_toast(d->toasts, "Ya existe un equipo de emergencia «" + nt + "»."); return false; }
        EquipoEmergencia x = idx < 0 ? EquipoEmergencia() : lista[size_t(idx)];
        x.tipo = TIPO_SAI; x.tag = nt; x.nombre = get_entry(nombre);
        x.horas = adw_spin_row_get_value(horas); x.eficienciaPct = adw_spin_row_get_value(efic);
        x.forzarTrifasico = adw_switch_row_get_active(tri);
        guint sc = adw_combo_row_get_selected(alim);
        x.cuadroAlimentacion = sc < tagsCuadro.size() ? tagsCuadro[sc] : "";
        if (idx < 0) lista.push_back(x);
        else {
            lista[size_t(idx)] = x;
            if (viejo != nt) recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) {
                if (c.tagEmer && *c.tagEmer == viejo) c.tagEmer = nt;
                if (c.saiAlimentacion == viejo) c.saiAlimentacion = nt;
                for (Carga& k: c.cargas) if (k.tagEmer && *k.tagEmer == viejo) k.tagEmer = nt;
            });
        }
        return true;
    };
    adw_dialog_present(d->dialog, a->window);
}

// ── Batería de condensadores (equipo del CGBT) ───────────────────────────────

// idxCgbt: índice del CGBT (cuadro de origen) cuya batería se edita; -1 = añadir una nueva
static void dialogo_bateria(AppState* a, int idxCgbt) {
    auto& cs = a->proyecto.cuadros;
    bool nueva = idxCgbt < 0;
    std::vector<int> candidatos;   // CGBT sin batería
    for (size_t i = 0; i < cs.size(); i++) if (cs[i].baterias.empty()) candidatos.push_back(int(i));
    if (nueva && candidatos.empty()) {
        show_toast(a->toasts, cs.empty() ? "Crea primero un cuadro general (CGBT)." : "Todos los CGBT tienen ya batería de condensadores.");
        return;
    }
    if (!nueva && (size_t(idxCgbt) >= cs.size() || cs[size_t(idxCgbt)].baterias.empty())) return;
    a->proyecto.recalcular();
    Bateria b = nueva ? Bateria() : cs[size_t(idxCgbt)].baterias.front();

    DlgSimple* d = dlg_simple(a, nueva ? "Nueva batería de condensadores" : "Batería de condensadores " + b.tag,
                              nueva ? "Añadir" : "Aplicar", 560);
    adw_preferences_group_set_description(d->grupo,
        "Sólo en un CGBT. Su potencia se calcula automáticamente con la P y la Q demandadas totales del CGBT "
        "(incluidos sus subcuadros) para alcanzar el cos φ objetivo, redondeando al escalón de regulación.");
    AdwComboRow* cgbt = nullptr;
    if (nueva) {
        std::vector<std::string> etiquetas;
        guint sel = 0;
        for (size_t k = 0; k < candidatos.size(); k++) {
            const Cuadro& c = cs[size_t(candidatos[k])];
            etiquetas.push_back(c.tag + " — " + c.nombre);
            if (!a->rutaSeleccionada.empty() && a->rutaSeleccionada.front() == size_t(candidatos[k])) sel = guint(k);
        }
        cgbt = combo_row(d->grupo, "CGBT", nullptr, etiquetas, sel);
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(cgbt), FALSE);
    } else {
        set_resultado(resultado_row(d->grupo, "CGBT"), cs[size_t(idxCgbt)].tag);
    }
    AdwSpinRow* cosObj = spin_row(d->grupo, "cos φ objetivo", nullptr, 0.80, 1.0, 0.01, 2, b.cosObjetivo);
    AdwSpinRow* escalon = spin_row(d->grupo, "Escalón de regulación [kVAr]", nullptr, 0.5, 500, 2.5, 1, b.escalonKvar);

    AdwPreferencesGroup* gR = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gR, "Resultado");
    GtkWidget* rNec = resultado_row(gR, "Q necesaria");
    GtkWidget* rQ = resultado_row(gR, "Potencia de la batería");
    GtkWidget* rCos = resultado_row(gR, "cos φ sin → con batería");
    adw_preferences_page_add(d->page, gR);
    auto cgbtElegido = [a, cgbt, candidatos, idxCgbt]() -> const Cuadro* {
        int i = idxCgbt;
        if (cgbt) { guint s = adw_combo_row_get_selected(cgbt); if (s < candidatos.size()) i = candidatos[s]; }
        return i >= 0 && size_t(i) < a->proyecto.cuadros.size() ? &a->proyecto.cuadros[size_t(i)] : nullptr;
    };
    auto previa = [cgbtElegido, cosObj, escalon, rNec, rQ, rCos] {
        const Cuadro* c = cgbtElegido();
        if (!c) return;
        double q = c->qKvar;
        for (const Bateria& x: c->baterias) q -= x.qKvar();   // Q del CGBT sin compensar
        Bateria t;
        t.cosObjetivo = adw_spin_row_get_value(cosObj);
        t.escalonKvar = adw_spin_row_get_value(escalon);
        t.calcular(c->pKw, q);
        set_resultado(rNec, fmt(t.qNecesariaKvar) + " kVAr");
        set_resultado(rQ, t.qKvarNominal > 0 ? fmt(t.qKvarNominal, 1) + " kVAr" : "No necesaria");
        set_resultado(rCos, fmt(t.cosAntes, 3) + " → " + fmt(t.cosDespues, 3));
    };
    previa();
    for (AdwSpinRow* r: {cosObj, escalon}) conectar_notify(r, "notify::value", previa);
    if (cgbt) conectar_notify(cgbt, "notify::selected", previa);

    d->aplicar = [a, cgbt, candidatos, idxCgbt, cosObj, escalon](DlgSimple*) {
        int i = idxCgbt;
        if (cgbt) { guint s = adw_combo_row_get_selected(cgbt); if (s >= candidatos.size()) return false; i = candidatos[s]; }
        Cuadro& c = a->proyecto.cuadros[size_t(i)];
        if (c.baterias.empty()) { Bateria nb; nb.nombre = "BATERÍA DE CONDENSADORES"; c.baterias.push_back(nb); }
        c.compensacion = true;
        c.baterias.front().cosObjetivo = adw_spin_row_get_value(cosObj);
        c.baterias.front().escalonKvar = adw_spin_row_get_value(escalon);
        return true;
    };
    adw_dialog_present(d->dialog, a->window);
}

static void borrar_bateria(AppState* a, size_t idxCgbt) {
    if (idxCgbt >= a->proyecto.cuadros.size() || a->proyecto.cuadros[idxCgbt].baterias.empty()) return;
    std::string tag = a->proyecto.cuadros[idxCgbt].baterias.front().tag, cgbt = a->proyecto.cuadros[idxCgbt].tag;
    confirmar(a, "¿Eliminar la batería " + tag + "?", "El CGBT " + cgbt + " quedará sin batería de condensadores.", [a, cgbt] {
        for (Cuadro& c: a->proyecto.cuadros)
            if (c.tag == cgbt) { c.baterias.clear(); c.compensacion = false; }
        a->modificado = true;
        refresh_all(a);
    });
}

// ── Fotovoltaica ────────────────────────────────────────────────────────────

static void dialogo_fv(AppState* a, int idx) {
    bool nuevo = idx < 0;
    InstalacionFV f;
    if (!nuevo) f = a->proyecto.datos.fotovoltaica[size_t(idx)];
    else {
        std::set<std::string> usados;
        for (const InstalacionFV& x: a->proyecto.datos.fotovoltaica) usados.insert(x.tag);
        int n = 1;
        while (usados.count("FV" + std::to_string(n))) n++;
        f.tag = "FV" + std::to_string(n); f.nombre = "INSTALACIÓN FOTOVOLTAICA " + std::to_string(n);
        f.pPicoKwp = 100; f.pInversorKw = 100;
        if (!a->proyecto.cuadros.empty()) f.cuadro = a->proyecto.cuadros.front().tag;
    }
    DlgSimple* d = dlg_simple(a, nuevo ? "Nueva instalación fotovoltaica" : "Fotovoltaica " + f.tag, nuevo ? "Añadir" : "Aplicar", 620);
    AdwEntryRow* tag = entry_row(d->grupo, "TAG");
    set_entry(tag, f.tag);
    AdwEntryRow* nombre = entry_row(d->grupo, "Nombre");
    set_entry(nombre, f.nombre);
    AdwSpinRow* pico = spin_row(d->grupo, "Potencia pico del campo [kWp]", nullptr, 0, 100000, 1, 2, f.pPicoKwp);
    AdwSpinRow* pInv = spin_row(d->grupo, "Potencia nominal por inversor [kW]", nullptr, 0, 10000, 1, 2, f.pInversorKw);
    AdwSpinRow* nInv = spin_row(d->grupo, "Número de inversores", nullptr, 1, 500, 1, 0, f.nInversores);
    const auto& fases = configuracionesFases();
    guint selF = guint(std::find(fases.begin(), fases.end(), f.configFases) - fases.begin());
    AdwComboRow* cf = combo_row(d->grupo, "Configuración de fases", nullptr, fases, selF < fases.size() ? selF : 2);

    std::vector<std::string> tags = {""}, etiquetas = {"Sin conexión"};
    guint selC = 0;
    std::vector<std::vector<size_t>> rutas;
    std::vector<std::string> lbls;
    recoger_cuadros(a->proyecto.cuadros, {}, 0, rutas, lbls);
    for (size_t i = 0; i < rutas.size(); i++) {
        const Cuadro* c = a->proyecto.cuadroPorRuta(rutas[i]);
        tags.push_back(c->tag); etiquetas.push_back(lbls[i]);
        if (c->tag == f.cuadro) selC = guint(tags.size() - 1);
    }
    AdwComboRow* cuadro = combo_row(d->grupo, "Cuadro de conexión", "Cuadro donde vierten los inversores", etiquetas, selC);

    AdwPreferencesGroup* gR = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gR, "Resultado");
    GtkWidget* rAc = resultado_row(gR, "Potencia AC total");
    GtkWidget* rRatio = resultado_row(gR, "Ratio DC/AC");
    adw_preferences_page_add(d->page, gR);
    auto actualizar = [pico, pInv, nInv, rAc, rRatio] {
        double ac = adw_spin_row_get_value(pInv) * adw_spin_row_get_value(nInv);
        set_resultado(rAc, fmt(ac) + " kW");
        set_resultado(rRatio, ac > 0 ? fmt(adw_spin_row_get_value(pico) / ac) : "—");
    };
    actualizar();
    for (AdwSpinRow* r: {pico, pInv, nInv})
        g_signal_connect_data(r, "notify::value",
            G_CALLBACK(+[](GObject*, GParamSpec*, gpointer fn) { (*static_cast<std::function<void()>*>(fn))(); }),
            new std::function<void()>(actualizar),
            [](gpointer fn, GClosure*) { delete static_cast<std::function<void()>*>(fn); }, GConnectFlags(0));

    d->aplicar = [a, idx, tag, nombre, pico, pInv, nInv, cf, cuadro, tags](DlgSimple* d) {
        std::string nt = get_entry(tag);
        auto& lista = a->proyecto.datos.fotovoltaica;
        if (nt.empty()) { show_toast(d->toasts, "El TAG es obligatorio."); return false; }
        for (size_t i = 0; i < lista.size(); i++)
            if (int(i) != idx && lista[i].tag == nt) { show_toast(d->toasts, "Ya existe una instalación «" + nt + "»."); return false; }
        InstalacionFV x = idx < 0 ? InstalacionFV() : lista[size_t(idx)];   // conserva la posición en el diagrama
        x.tag = nt; x.nombre = get_entry(nombre);
        x.pPicoKwp = adw_spin_row_get_value(pico); x.pInversorKw = adw_spin_row_get_value(pInv);
        x.nInversores = int(adw_spin_row_get_value(nInv));
        guint sf = adw_combo_row_get_selected(cf);
        if (sf < configuracionesFases().size()) x.configFases = configuracionesFases()[sf];
        guint sc = adw_combo_row_get_selected(cuadro);
        x.cuadro = sc < tags.size() ? tags[sc] : "";
        if (idx < 0) lista.push_back(x); else lista[size_t(idx)] = x;
        return true;
    };
    adw_dialog_present(d->dialog, a->window);
}

static void borrar_fv(AppState* a, size_t idx) {
    std::string tag = a->proyecto.datos.fotovoltaica[idx].tag;
    confirmar(a, "¿Eliminar la instalación fotovoltaica " + tag + "?", "", [a, tag] {
        auto& l = a->proyecto.datos.fotovoltaica;
        l.erase(std::remove_if(l.begin(), l.end(), [&](const InstalacionFV& x) { return x.tag == tag; }), l.end());
        a->modificado = true;
        refresh_all(a);
    });
}

// ── Zonas ───────────────────────────────────────────────────────────────────

static bool tag_simple_valido(const std::string& t) {
    if (t.empty()) return false;
    for (unsigned char ch: t) if (!std::isalnum(ch) && ch != '_') return false;   // sin guiones: separan los campos del TAG
    return true;
}

static void dialogo_zona(AppState* a, int idx) {
    Zona z;
    if (idx >= 0) z = a->proyecto.datos.zonas[size_t(idx)];
    DlgSimple* d = dlg_simple(a, idx < 0 ? "Nueva zona" : "Zona " + z.tag, idx < 0 ? "Añadir" : "Aplicar", 340);
    adw_preferences_group_set_description(d->grupo, "El TAG de la zona encabeza el TAG de las cargas: &lt;zona&gt;-&lt;tipo&gt;-&lt;nº&gt;");
    AdwEntryRow* tag = entry_row(d->grupo, "TAG (p. ej. PS)");
    set_entry(tag, z.tag);
    AdwEntryRow* desc = entry_row(d->grupo, "Descripción");
    set_entry(desc, z.descripcion);
    d->aplicar = [a, idx, tag, desc, viejo = z.tag](DlgSimple* d) {
        std::string nt = get_entry(tag);
        std::transform(nt.begin(), nt.end(), nt.begin(), ::toupper);
        auto& lista = a->proyecto.datos.zonas;
        if (!tag_simple_valido(nt)) { show_toast(d->toasts, "El TAG de zona sólo puede tener letras y números (sin guiones)."); return false; }
        for (size_t i = 0; i < lista.size(); i++)
            if (int(i) != idx && lista[i].tag == nt) { show_toast(d->toasts, "Ya existe la zona «" + nt + "»."); return false; }
        Zona x{nt, get_entry(desc)};
        if (idx < 0) lista.push_back(x);
        else {
            lista[size_t(idx)] = x;
            if (viejo != nt) recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) {
                if (c.zona == viejo) c.zona = nt;
                for (Carga& k: c.cargas) if (k.zona == viejo) k.zona = nt;
            });
        }
        return true;
    };
    adw_dialog_present(d->dialog, a->window);
}

static void borrar_zona(AppState* a, size_t idx) {
    std::string tag = a->proyecto.datos.zonas[idx].tag;
    int usos = 0;
    int usosCargas = 0;
    recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) {
        if (c.zona == tag) usos++;
        for (const Carga& k: c.cargas) if (k.zona == tag) usosCargas++;
    });
    confirmar(a, "¿Eliminar la zona " + tag + "?",
              (usos || usosCargas)
                  ? std::to_string(usos) + " cuadro(s) y " + std::to_string(usosCargas) + " carga(s) usan esta zona; las cargas pasarán a la zona de su cuadro."
                  : "Ni cuadros ni cargas usan esta zona.",
              [a, tag] {
        auto& l = a->proyecto.datos.zonas;
        l.erase(std::remove_if(l.begin(), l.end(), [&](const Zona& z) { return z.tag == tag; }), l.end());
        recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) {
            if (c.zona == tag) c.zona.clear();
            for (Carga& k: c.cargas) if (k.zona == tag) k.zona.clear();
        });
        a->modificado = true;
        refresh_all(a);
    });
}

// ── Tipos de carga (tabla editable) ─────────────────────────────────────────

enum ColTipo { T_NOMBRE, T_PREFIJO, T_DESC, T_COS, T_ETA, T_USO, N_COLT };
static const int FILA_BATERIA = -1;   // última fila: batería de condensadores (sólo prefijo)
static const char* MODOS_ETA[3] = {"No aplica", "P absorbida = P/η", "Informativo"};

struct CeldaTipo { AppState* a; int col; };
struct CambioTipo { AppState* a; int fila; int col; std::string valor; };

static int cargas_de_tipo(const Proyecto& p, const std::string& tipo) {
    int n = 0;
    std::function<void(const std::vector<Cuadro>&)> v = [&](const std::vector<Cuadro>& cs) {
        for (const Cuadro& c: cs) {
            if (tipo == TIPO_BATERIA) n += int(c.baterias.size());
            else for (const Carga& k: c.cargas) if (k.tipo == tipo) n++;
            v(c.subcuadros);
        }
    };
    v(p.cuadros);
    return n;
}

static std::string texto_tipo(const Proyecto& p, int fila, int col) {
    if (fila == FILA_BATERIA) {
        switch (col) {
            case T_PREFIJO: return p.datos.prefijo(TIPO_BATERIA);
            case T_DESC: return p.datos.descripcionTipo(TIPO_BATERIA) + " (sólo en el CGBT)";
            case T_USO: return std::to_string(cargas_de_tipo(p, TIPO_BATERIA));
            default: return "—";
        }
    }
    const TipoCargaDef& t = p.datos.tiposCarga[size_t(fila)];
    switch (col) {
        case T_NOMBRE: return t.nombre;
        case T_PREFIJO: return t.prefijo;
        case T_DESC: return t.descripcion;
        case T_COS: return t.cosFijo ? fmt(*t.cosFijo, 2) : "tabla";
        case T_USO: return std::to_string(cargas_de_tipo(p, t.nombre));
        default: return "";
    }
}

static void aplicar_tipo(AppState* a, int fila, int col, std::string v) {
    auto& tipos = a->proyecto.datos.tiposCarga;
    auto error = [&](const std::string& m) { show_toast(a->toastsTipos ? a->toastsTipos : a->toasts, m); refresh_general(a); };
    v = trim(v);
    if (fila != FILA_BATERIA && (fila < 0 || size_t(fila) >= tipos.size())) return;
    if (col == T_NOMBRE || col == T_PREFIJO) {
        // a mayúsculas (también acentos)
        gchar* up = g_utf8_strup(v.c_str(), -1);
        v = up;
        g_free(up);
    }
    switch (col) {
        case T_NOMBRE: {
            std::string viejo = tipos[size_t(fila)].nombre;
            if (v == viejo) return;
            if (v.empty()) return error("El nombre del tipo es obligatorio.");
            if (v == TIPO_BATERIA || a->proyecto.datos.tipoCarga(v)) return error("Ya existe un tipo «" + v + "».");
            tipos[size_t(fila)].nombre = v;
            recorrer_cuadros(a->proyecto.cuadros, [&](Cuadro& c) { for (Carga& k: c.cargas) if (k.tipo == viejo) k.tipo = v; });
            if (a->ultimoTipoCarga == viejo) a->ultimoTipoCarga = v;
            break;
        }
        case T_PREFIJO:
            if (!tag_simple_valido(v)) return error("El TAG del tipo sólo puede tener letras y números (sin guiones).");
            if (fila == FILA_BATERIA) { if (v == a->proyecto.datos.prefijoBateria) return; a->proyecto.datos.prefijoBateria = v; }
            else { if (v == tipos[size_t(fila)].prefijo) return; tipos[size_t(fila)].prefijo = v; }
            break;
        case T_DESC: {
            // La descripción identifica al tipo al elegirlo en las cargas: obligatoria y única
            if (v == tipos[size_t(fila)].descripcion) return;
            if (v.empty()) return error("La descripción es obligatoria: es lo que se elige en las cargas.");
            auto clave = [](const std::string& t) {   // comparación sin distinguir mayúsculas
                gchar* f = g_utf8_casefold(t.c_str(), -1);
                std::string r = f;
                g_free(f);
                return r;
            };
            for (size_t i = 0; i < tipos.size(); i++)
                if (int(i) != fila && clave(tipos[i].descripcion) == clave(v))
                    return error("Ya hay un tipo de carga «" + tipos[i].descripcion + "».");
            tipos[size_t(fila)].descripcion = v;
            break;
        }
        case T_COS: {
            std::string l = v;
            std::transform(l.begin(), l.end(), l.begin(), ::tolower);
            std::optional<double> nuevo;
            if (!(l.empty() || l == "tabla" || l == "auto" || l == "tabla motores")) {
                std::replace(l.begin(), l.end(), ',', '.');
                char* fin = nullptr;
                double x = g_ascii_strtod(l.c_str(), &fin);
                if (!fin || *fin || !(x > 0 && x <= 1)) return error("cos φ: un número entre 0 y 1, o «tabla» para usar la tabla de motores.");
                nuevo = x;
            }
            if (nuevo == tipos[size_t(fila)].cosFijo) return;
            tipos[size_t(fila)].cosFijo = nuevo;
            break;
        }
        case T_ETA: {
            int m = std::atoi(v.c_str());
            if (m == tipos[size_t(fila)].rendimiento) return;
            tipos[size_t(fila)].rendimiento = m;
            break;
        }
        default: return;
    }
    a->modificado = true;
    refresh_all(a);
}

// Los cambios se aplican fuera de la señal de la celda (la tabla se reconstruye al aplicar)
static void aplicar_tipo_diferido(AppState* a, int fila, int col, const std::string& valor) {
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto* c = static_cast<CambioTipo*>(p);
        aplicar_tipo(c->a, c->fila, c->col, c->valor);
        return G_SOURCE_REMOVE;
    }, new CambioTipo{a, fila, col, valor}, [](gpointer p) { delete static_cast<CambioTipo*>(p); });
}

static void tipo_setup(GtkSignalListItemFactory*, GObject* obj, gpointer data) {
    auto* ct = static_cast<CeldaTipo*>(data);
    GtkListItem* li = GTK_LIST_ITEM(obj);
    GtkWidget* w;
    if (ct->col == T_ETA) {
        // Botón que abre la elección en un diálogo (un desplegable dentro de la tabla no llega a abrirse)
        w = gtk_button_new();
        GtkWidget* c = adw_button_content_new();
        adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(c), "pan-down-symbolic");
        gtk_button_set_child(GTK_BUTTON(w), c);
        gtk_widget_add_css_class(w, "flat");
        g_signal_connect(w, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
            auto* ct = static_cast<CeldaTipo*>(data);
            int fila = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "fila")) - 2;
            if (fila < 0) return;
            const TipoCargaDef& t = ct->a->proyecto.datos.tiposCarga[size_t(fila)];
            AdwAlertDialog* d = ADW_ALERT_DIALOG(adw_alert_dialog_new(("Rendimiento: " + t.descripcion).c_str(),
                "«P absorbida = P/η»: la potencia absorbida es la nominal dividida por el rendimiento (motores).\n"
                "«Informativo»: se calcula y muestra el rendimiento, pero no afecta a la potencia (p. ej. con variador)."));
            adw_alert_dialog_add_response(d, "cancelar", "Cancelar");
            for (int k = 0; k < 3; k++) adw_alert_dialog_add_response(d, std::to_string(k).c_str(), MODOS_ETA[k]);
            adw_alert_dialog_set_response_appearance(d, std::to_string(t.rendimiento).c_str(), ADW_RESPONSE_SUGGESTED);
            adw_alert_dialog_set_prefer_wide_layout(d, TRUE);
            adw_alert_dialog_set_close_response(d, "cancelar");
            g_object_set_data(G_OBJECT(d), "fila", GINT_TO_POINTER(fila + 2));
            g_signal_connect(d, "response", G_CALLBACK(+[](AdwAlertDialog* d, const char* resp, gpointer data) {
                if (g_strcmp0(resp, "cancelar") == 0) return;
                auto* ct = static_cast<CeldaTipo*>(data);
                aplicar_tipo_diferido(ct->a, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(d), "fila")) - 2, T_ETA, resp);
            }), ct);
            adw_dialog_present(ADW_DIALOG(d), ct->a->window);
        }), ct);
    } else if (ct->col == T_USO) {
        w = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(w), 1.0);
        gtk_widget_add_css_class(w, "numeric");
        gtk_widget_add_css_class(w, "dim-label");
    } else {
        w = gtk_editable_label_new("");
        g_signal_connect(w, "notify::editing", G_CALLBACK(+[](GtkEditableLabel* el, GParamSpec*, gpointer data) {
            if (gtk_editable_label_get_editing(el)) return;
            auto* ct = static_cast<CeldaTipo*>(data);
            const char* orig = static_cast<const char*>(g_object_get_data(G_OBJECT(el), "original"));
            std::string texto = gtk_editable_get_text(GTK_EDITABLE(el));
            if (orig && texto == orig) return;
            int fila = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(el), "fila")) - 2;
            aplicar_tipo_diferido(ct->a, fila, ct->col, texto);
        }), ct);
    }
    gtk_list_item_set_child(li, w);
}

static void tipo_bind(GtkSignalListItemFactory*, GObject* obj, gpointer data) {
    auto* ct = static_cast<CeldaTipo*>(data);
    GtkListItem* li = GTK_LIST_ITEM(obj);
    int fila = std::atoi(gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(li))));
    GtkWidget* w = gtk_list_item_get_child(li);
    const Proyecto& p = ct->a->proyecto;
    g_object_set_data(G_OBJECT(w), "fila", GINT_TO_POINTER(fila + 2));   // +2: el 0 se reserva para «sin dato»
    bool bateria = fila == FILA_BATERIA;
    if (ct->col == T_ETA) {
        int m = bateria ? 0 : std::clamp(p.datos.tiposCarga[size_t(fila)].rendimiento, 0, 2);
        adw_button_content_set_label(ADW_BUTTON_CONTENT(gtk_button_get_child(GTK_BUTTON(w))), bateria ? "—" : MODOS_ETA[m]);
        gtk_widget_set_sensitive(w, !bateria);
    } else if (ct->col == T_USO) {
        gtk_label_set_text(GTK_LABEL(w), texto_tipo(p, fila, ct->col).c_str());
    } else {
        std::string t = texto_tipo(p, fila, ct->col);
        g_object_set_data_full(G_OBJECT(w), "original", g_strdup(t.c_str()), g_free);
        gtk_editable_set_text(GTK_EDITABLE(w), t.c_str());
        gtk_widget_set_sensitive(w, !bateria || ct->col == T_PREFIJO);
        gtk_widget_set_tooltip_text(w, ct->col == T_COS ? "Número entre 0 y 1, o «tabla» (cos φ según la tabla de motores)"
                                                        : "Haz clic para editar; Intro para aplicar");
    }
}

static GtkWidget* construir_tabla_tipos(AppState* a) {
    GtkWidget* cv = gtk_column_view_new(nullptr);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(cv), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(cv), TRUE);
    gtk_widget_add_css_class(cv, "data-table");
    struct Def { const char* titulo; int col; int ancho; bool expandir; };
    for (const Def& d: {Def{"TAG", T_PREFIJO, 70, false}, Def{"Descripción", T_DESC, 330, false},
                        Def{"cos φ", T_COS, 65, false}, Def{"Rendimiento", T_ETA, 175, false}, Def{"Cargas", T_USO, 55, false}}) {
        GtkListItemFactory* f = gtk_signal_list_item_factory_new();
        auto* ct = new CeldaTipo{a, d.col};   // vive mientras la aplicación
        g_signal_connect(f, "setup", G_CALLBACK(tipo_setup), ct);
        g_signal_connect(f, "bind", G_CALLBACK(tipo_bind), ct);
        GtkColumnViewColumn* c = gtk_column_view_column_new(d.titulo, f);
        gtk_column_view_column_set_resizable(c, TRUE);
        if (d.ancho) gtk_column_view_column_set_fixed_width(c, d.ancho);
        gtk_column_view_column_set_expand(c, d.expandir);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(cv), c);
        g_object_unref(c);
    }
    return cv;
}

static void refrescar_tabla_tipos(AppState* a) {
    if (!a->tablaTipos) return;
    GtkStringList* filas = gtk_string_list_new(nullptr);
    for (size_t i = 0; i < a->proyecto.datos.tiposCarga.size(); i++) gtk_string_list_append(filas, std::to_string(i).c_str());
    gtk_string_list_append(filas, std::to_string(FILA_BATERIA).c_str());
    GtkSingleSelection* sel = gtk_single_selection_new(G_LIST_MODEL(filas));
    gtk_single_selection_set_autoselect(sel, FALSE);
    gtk_single_selection_set_can_unselect(sel, TRUE);
    gtk_single_selection_set_selected(sel, GTK_INVALID_LIST_POSITION);
    gtk_column_view_set_model(GTK_COLUMN_VIEW(a->tablaTipos), GTK_SELECTION_MODEL(sel));
    g_object_unref(sel);
}

static int fila_tipo_seleccionada(AppState* a) {
    auto* sel = GTK_SINGLE_SELECTION(gtk_column_view_get_model(GTK_COLUMN_VIEW(a->tablaTipos)));
    gpointer item = gtk_single_selection_get_selected_item(sel);
    return item ? std::atoi(gtk_string_object_get_string(GTK_STRING_OBJECT(item))) : -2;
}

static void anadir_tipo(AppState* a) {
    auto& tipos = a->proyecto.datos.tiposCarga;
    std::string nombre = "TIPO1";   // identificador interno (no se muestra)
    for (int n = 2; a->proyecto.datos.tipoCarga(nombre); n++) nombre = "TIPO" + std::to_string(n);
    std::string desc = "NUEVO TIPO";
    auto existe = [&](const std::string& d) { return std::any_of(tipos.begin(), tipos.end(), [&](const TipoCargaDef& t) { return t.descripcion == d; }); };
    for (int n = 2; existe(desc); n++) desc = "NUEVO TIPO " + std::to_string(n);
    tipos.push_back({nombre, "NUE", desc, 0.85, 0});
    a->modificado = true;
    refresh_all(a);
    show_toast(a->toastsTipos ? a->toastsTipos : a->toasts, "Tipo «" + desc + "» añadido: haz clic en sus celdas para editarlo");
}

static void eliminar_tipo(AppState* a) {
    int fila = fila_tipo_seleccionada(a);
    auto& tipos = a->proyecto.datos.tiposCarga;
    if (fila == -2) { show_toast(a->toastsTipos ? a->toastsTipos : a->toasts, "Selecciona en la tabla el tipo que quieres eliminar."); return; }
    if (fila == FILA_BATERIA) { show_toast(a->toastsTipos ? a->toastsTipos : a->toasts, "La batería de condensadores no es un tipo de carga; sólo se puede cambiar su TAG."); return; }
    std::string nombre = tipos[size_t(fila)].nombre, desc = tipos[size_t(fila)].descripcion;
    int n = cargas_de_tipo(a->proyecto, nombre);
    if (n > 0) { show_toast(a->toastsTipos ? a->toastsTipos : a->toasts, "Hay " + std::to_string(n) + " carga(s) de tipo «" + desc + "»: cámbialas de tipo antes de eliminarlo."); return; }
    tipos.erase(tipos.begin() + fila);
    a->modificado = true;
    refresh_all(a);
    show_toast(a->toastsTipos ? a->toastsTipos : a->toasts, "Tipo «" + desc + "» eliminado");
}

static void restablecer_tipos(AppState* a) {
    confirmar(a, "¿Restablecer los tipos de carga de serie?",
              "Se perderán los cambios en TAG, descripciones, cos φ y rendimiento. Los tipos que no son de serie "
              "y tienen cargas se conservan.", [a] {
        auto& dg = a->proyecto.datos;
        std::vector<TipoCargaDef> nuevos = tiposCargaDeSerie();
        for (const TipoCargaDef& t: dg.tiposCarga) {
            bool deSerie = std::any_of(nuevos.begin(), nuevos.end(), [&](const TipoCargaDef& x) { return x.nombre == t.nombre; });
            if (!deSerie && cargas_de_tipo(a->proyecto, t.nombre) > 0) nuevos.push_back(t);
        }
        dg.tiposCarga = nuevos;
        dg.prefijoBateria = "CAP";
        a->modificado = true;
        refresh_all(a);
    });
}

// ── Página ──────────────────────────────────────────────────────────────────

static void refresh_general(AppState* a) {
    if (!a->listaTrafos) return;
    const Proyecto& p = a->proyecto;

    vaciar_lista(a->listaZonas);
    for (size_t i = 0; i < p.datos.zonas.size(); i++) {
        const Zona& z = p.datos.zonas[i];
        int usos = 0;
        int usosCargas = 0;
        std::function<void(const std::vector<Cuadro>&)> contar = [&](const std::vector<Cuadro>& cs) {
            for (const Cuadro& c: cs) {
                if (c.zona == z.tag) usos++;
                for (const Carga& k: c.cargas) if (k.zona == z.tag) usosCargas++;
                contar(c.subcuadros);
            }
        };
        contar(p.cuadros);
        std::string sub = z.descripcion + (z.descripcion.empty() ? "" : " · ") + std::to_string(usos) + " cuadro(s) · " + std::to_string(usosCargas) + " carga(s) con zona propia · p. ej. " +
                          z.tag + "-" + p.datos.prefijo("MOTOR") + "-01";
        gtk_list_box_append(GTK_LIST_BOX(a->listaZonas), fila_editable(z.tag, sub, "",
            [a, i] { dialogo_zona(a, int(i)); }, [a, i] { borrar_zona(a, i); }));
    }
    if (p.datos.zonas.empty()) gtk_list_box_append(GTK_LIST_BOX(a->listaZonas), fila_vacia("Sin zonas"));

    refrescar_tabla_tipos(a);
    if (a->resumenTipos) {
        std::string sub = std::to_string(p.datos.tiposCarga.size()) + " tipos · batería de condensadores: " + p.datos.prefijo(TIPO_BATERIA);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(a->resumenTipos), sub.c_str());
    }

    vaciar_lista(a->listaTrafos);
    for (size_t i = 0; i < p.datos.transformadores.size(); i++) {
        const Transformador& t = p.datos.transformadores[i];
        double pk = 0, qk = 0;
        std::string alimenta;
        for (const Cuadro& c: p.cuadros) {
            if (c.trafoEfectivo != t.tag) continue;
            pk += c.pKw; qk += c.qKvar;
            alimenta += (alimenta.empty() ? "" : ", ") + c.tag;
        }
        double s = std::hypot(pk, qk), ref = t.sAdmKva > 0 ? t.sAdmTotal() : t.sNomTotal();
        std::string sub = (t.nParalelo > 1 ? std::to_string(t.nParalelo) + " × " : "") + fmt(t.sNomKva, 0) + " kVA";
        if (t.nParalelo > 1) sub += " = " + fmt(t.sNomTotal(), 0) + " kVA";
        sub += " (admisible " + fmt(t.sAdmTotal(), 0) + " kVA) · " + fmt(t.uSecV, 0) + " V";
        sub += alimenta.empty() ? " · sin cuadros" : " · alimenta " + alimenta + " · S " + fmt(s, 1) + " kVA";
        if (ref > 0 && !alimenta.empty()) sub += " (" + fmt(100 * s / ref, 1) + " %)";
        std::string aviso = (ref > 0 && s > ref) ? "La S demandada supera la potencia admisible" : "";
        gtk_list_box_append(GTK_LIST_BOX(a->listaTrafos), fila_editable(t.tag, sub, aviso,
            [a, i] { dialogo_trafo(a, int(i)); }, [a, i] { borrar_trafo(a, i); }));
    }
    if (p.datos.transformadores.empty())
        gtk_list_box_append(GTK_LIST_BOX(a->listaTrafos), fila_vacia("Sin transformadores"));

    vaciar_lista(a->listaGE);
    bool hayGE = false;
    for (size_t i = 0; i < p.equipos.size(); i++) {
        const EquipoEmergencia& e = p.equipos[i];
        if (e.tipo != TIPO_GE) continue;
        hayGE = true;
        std::string sub = "S emergencia " + fmt(e.sEmerKva, 1) + " kVA";
        if (e.sDisenoKva) sub += " · grupo de " + fmt(*e.sDisenoKva, 0) + " kVA";
        sub += " · " + e.fases + " · " + std::to_string(e.nConsumidores) + " consumidor(es) · " + fmt(e.horas, 1) + " h";
        if (e.vDepositoAdL && *e.vDepositoAdL > 0) sub += " · depósito adicional " + fmt(*e.vDepositoAdL, 0) + " L";
        gtk_list_box_append(GTK_LIST_BOX(a->listaGE), fila_editable(e.tag + (e.nombre.empty() ? "" : " — " + e.nombre), sub,
            unir(e.avisos), [a, i] { dialogo_ge(a, int(i)); }, [a, i] { borrar_ge(a, i); }));
    }
    if (!hayGE) gtk_list_box_append(GTK_LIST_BOX(a->listaGE), fila_vacia("Sin grupos electrógenos"));

    vaciar_lista(a->listaSAI);
    bool haySAI = false;
    for (size_t i = 0; i < p.equipos.size(); i++) {
        const EquipoEmergencia& e = p.equipos[i];
        if (e.tipo != TIPO_SAI) continue;
        haySAI = true;
        std::string sub = "S emergencia " + fmt(e.sEmerKva, 1) + " kVA";
        if (e.sDisenoKva) sub += " · SAI de " + fmt(*e.sDisenoKva, 1) + " kVA";
        sub += " · " + e.fases + " · " + std::to_string(e.nConsumidores) + " consumidor(es) · " + fmt(e.horas, 2) + " h";
        if (e.capBateriaKwh) sub += " · baterías " + fmt(*e.capBateriaKwh) + " kWh";
        if (!e.cuadroAlimentacion.empty()) sub += " · alimentado desde " + e.cuadroAlimentacion;
        gtk_list_box_append(GTK_LIST_BOX(a->listaSAI), fila_editable(e.tag + (e.nombre.empty() ? "" : " — " + e.nombre), sub,
            unir(e.avisos), [a, i] { dialogo_sai(a, int(i)); }, [a, i] { borrar_ge(a, i); }));
    }
    if (!haySAI) gtk_list_box_append(GTK_LIST_BOX(a->listaSAI), fila_vacia("Sin SAI"));

    vaciar_lista(a->listaFV);
    for (size_t i = 0; i < p.datos.fotovoltaica.size(); i++) {
        const InstalacionFV& f = p.datos.fotovoltaica[i];
        std::string sub = fmt(f.pPicoKwp, 1) + " kWp · " + std::to_string(f.nInversores) + " × " + fmt(f.pInversorKw, 1) +
                          " kW = " + fmt(f.pAcKw, 1) + " kW AC · ratio " + fmt(f.ratioDcAc) + " · " + f.configFases;
        if (!f.cuadro.empty()) sub += " · conectada a " + f.cuadro;
        gtk_list_box_append(GTK_LIST_BOX(a->listaFV), fila_editable(f.tag + (f.nombre.empty() ? "" : " — " + f.nombre), sub,
            unir(f.avisos), [a, i] { dialogo_fv(a, int(i)); }, [a, i] { borrar_fv(a, i); }));
    }
    if (p.datos.fotovoltaica.empty())
        gtk_list_box_append(GTK_LIST_BOX(a->listaFV), fila_vacia("Sin instalaciones fotovoltaicas"));
}

// Ventana aparte para editar los tipos de carga (tabla)
static void abrir_ventana_tipos(AppState* a) {
    AdwDialog* d = adw_dialog_new();
    adw_dialog_set_title(d, "Tipos de carga");
    adw_dialog_set_content_width(d, 820);
    adw_dialog_set_content_height(d, 640);
    AdwToolbarView* tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar* hb = ADW_HEADER_BAR(adw_header_bar_new());
    GtkWidget* bAnadir = gtk_button_new_from_icon_name("list-add-symbolic");
    GtkWidget* bQuitar = gtk_button_new_from_icon_name("list-remove-symbolic");
    GtkWidget* bReset = gtk_button_new_from_icon_name("edit-undo-symbolic");
    gtk_widget_set_tooltip_text(bAnadir, "Añadir un tipo de carga");
    gtk_widget_set_tooltip_text(bQuitar, "Eliminar el tipo seleccionado (si no tiene cargas)");
    gtk_widget_set_tooltip_text(bReset, "Restablecer los tipos de serie");
    conectar_clic(bAnadir, [a] { anadir_tipo(a); });
    conectar_clic(bQuitar, [a] { eliminar_tipo(a); });
    conectar_clic(bReset, [a] { restablecer_tipos(a); });
    adw_header_bar_pack_start(hb, bAnadir);
    adw_header_bar_pack_start(hb, bQuitar);
    adw_header_bar_pack_start(hb, bReset);
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));

    GtkWidget* caja = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* ayuda = gtk_label_new("TAG de las cargas = <zona>-<TAG del tipo>-<nº de 2 dígitos>, p. ej. PS-M-01. Haz clic en una celda para "
                                     "editarla e Intro para aplicar. La descripción es lo que se elige en cada carga. cos φ: número o "
                                     "«tabla» (tabla de motores).");
    gtk_label_set_wrap(GTK_LABEL(ayuda), TRUE);
    gtk_label_set_xalign(GTK_LABEL(ayuda), 0);
    gtk_widget_add_css_class(ayuda, "dim-label");
    gtk_widget_set_margin_start(ayuda, 12); gtk_widget_set_margin_end(ayuda, 12); gtk_widget_set_margin_top(ayuda, 6);
    gtk_box_append(GTK_BOX(caja), ayuda);
    a->tablaTipos = construir_tabla_tipos(a);
    GtkWidget* desplazar = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(desplazar), a->tablaTipos);
    gtk_widget_set_vexpand(desplazar, TRUE);
    gtk_widget_add_css_class(desplazar, "view");
    gtk_box_append(GTK_BOX(caja), desplazar);
    gtk_widget_add_css_class(GTK_WIDGET(tv), "background");   // fondo opaco de la ventana
    a->toastsTipos = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(a->toastsTipos, caja);
    adw_toolbar_view_set_content(tv, GTK_WIDGET(a->toastsTipos));
    adw_dialog_set_child(d, GTK_WIDGET(tv));
    g_signal_connect(d, "closed", G_CALLBACK(+[](AdwDialog*, gpointer data) {
        auto* a = static_cast<AppState*>(data);
        a->tablaTipos = nullptr;
        a->toastsTipos = nullptr;
    }), a);
    refrescar_tabla_tipos(a);
    adw_dialog_present(d, a->window);
}

static GtkWidget* grupo_general(AdwPreferencesPage* page, const char* titulo, const char* desc, const char* icono,
                                const char* boton, std::function<void()> anadir) {
    AdwPreferencesGroup* g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, titulo);
    if (desc) adw_preferences_group_set_description(g, desc);
    GtkWidget* b = gtk_button_new();
    GtkWidget* c = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(c), icono);
    adw_button_content_set_label(ADW_BUTTON_CONTENT(c), boton);
    gtk_button_set_child(GTK_BUTTON(b), c);
    gtk_widget_add_css_class(b, "flat");
    conectar_clic(b, std::move(anadir));
    adw_preferences_group_set_header_suffix(g, b);
    GtkWidget* lista = nueva_lista();
    adw_preferences_group_add(g, lista);
    adw_preferences_page_add(page, g);
    return lista;
}

static GtkWidget* construir_general(AppState* a) {
    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    a->listaZonas = grupo_general(page, "Zonas",
        "Se asignan a los cuadros y forman el TAG de las cargas: &lt;zona&gt;-&lt;tipo&gt;-&lt;nº&gt;",
        "list-add-symbolic", "Añadir", [a] { dialogo_zona(a, -1); });
    a->listaTrafos = grupo_general(page, "Transformadores",
        "Se asignan a los cuadros generales (CGBT) desde la arquitectura eléctrica",
        "list-add-symbolic", "Añadir", [a] { dialogo_trafo(a, -1); });
    a->listaGE = grupo_general(page, "Grupos electrógenos",
        "Las cargas y cuadros se asocian a un grupo desde su equipo de emergencia",
        "list-add-symbolic", "Añadir", [a] { dialogo_ge(a, -1); });
    a->listaSAI = grupo_general(page, "SAI (sistemas de alimentación ininterrumpida)",
        "Las cargas y cuadros se asocian a un SAI desde su equipo de emergencia",
        "list-add-symbolic", "Añadir", [a] { dialogo_sai(a, -1); });
    a->listaFV = grupo_general(page, "Fotovoltaica", nullptr,
        "list-add-symbolic", "Añadir", [a] { dialogo_fv(a, -1); });

    AdwPreferencesGroup* gP = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gP, "Tipos de carga");
    a->resumenTipos = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(a->resumenTipos), "Tipos de carga y sus TAG");
    GtkWidget* bTipos = gtk_button_new_with_label("Editar…");
    gtk_widget_set_valign(bTipos, GTK_ALIGN_CENTER);
    conectar_clic(bTipos, [a] { abrir_ventana_tipos(a); });
    adw_action_row_add_suffix(ADW_ACTION_ROW(a->resumenTipos), bTipos);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(a->resumenTipos), bTipos);
    adw_preferences_group_add(gP, a->resumenTipos);
    adw_preferences_page_add(page, gP);
    return GTK_WIDGET(page);
}

// Mover las cargas seleccionadas en la lista (Ctrl/Mayús + clic para varias) a otro cuadro
static void mover_cargas(AppState* a) {
    GtkSelectionModel* m = gtk_column_view_get_model(GTK_COLUMN_VIEW(a->archView));
    std::vector<std::pair<std::vector<size_t>, int>> sel;
    std::string tags;
    GtkBitset* b = gtk_selection_model_get_selection(m);
    GtkBitsetIter it;
    guint pos;
    for (bool ok = gtk_bitset_iter_init_first(&it, b, &pos); ok; ok = gtk_bitset_iter_next(&it, &pos)) {
        GObject* row = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(m), pos));
        ArchNode* n = nodo_de_fila(row);
        if (n && n->tipo == Nodo::Carga) {
            sel.push_back({n->ruta, n->idx});
            if (sel.size() <= 6) tags += (tags.empty() ? "" : ", ") + n->col[C_ELEMENTO];
        }
        if (row) g_object_unref(row);
    }
    gtk_bitset_unref(b);
    if (sel.empty()) { show_toast(a->toasts, "Selecciona en la lista las cargas que quieres mover (Ctrl o Mayús + clic para varias)."); return; }
    if (sel.size() > 6) tags += "…";

    DlgSimple* d = dlg_simple(a, "Mover cargas", "Mover", 330);
    adw_preferences_group_set_description(d->grupo, (std::to_string(sel.size()) + " carga(s): " + tags).c_str());
    std::vector<std::vector<size_t>> rutas;
    std::vector<std::string> etiquetas;
    recoger_cuadros(a->proyecto.cuadros, {}, 0, rutas, etiquetas);
    AdwComboRow* destino = combo_row(d->grupo, "Cuadro de destino", nullptr, etiquetas, 0);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(destino), FALSE);
    habilitar_busqueda(destino);
    d->aplicar = [a, sel, rutas, destino](DlgSimple*) {
        guint k = adw_combo_row_get_selected(destino);
        if (k >= rutas.size()) return false;
        // Copias en el orden de la lista; después se borran de sus cuadros (índices de mayor a menor)
        std::vector<Carga> movidas;
        std::map<std::vector<size_t>, std::vector<int>> porCuadro;
        for (auto& [r, i]: sel) {
            Cuadro* c = a->proyecto.cuadroPorRuta(r);
            if (!c || i < 0 || size_t(i) >= c->cargas.size()) continue;
            movidas.push_back(c->cargas[size_t(i)]);
            porCuadro[r].push_back(i);
        }
        for (auto& [r, idxs]: porCuadro) {
            std::sort(idxs.rbegin(), idxs.rend());
            Cuadro* c = a->proyecto.cuadroPorRuta(r);
            for (int i: idxs) c->cargas.erase(c->cargas.begin() + i);
        }
        Cuadro* dest = a->proyecto.cuadroPorRuta(rutas[k]);
        for (Carga& c: movidas) dest->cargas.push_back(c);
        a->rutaSeleccionada = rutas[k];
        show_toast(a->toasts, std::to_string(movidas.size()) + " carga(s) movidas a " + dest->tag);
        return true;
    };
    adw_dialog_present(d->dialog, a->window);
}

static void on_add_load(GtkWidget*, gpointer data) {
    abrir_dialogo_carga(static_cast<AppState*>(data));
}

static void on_arch_activate(GtkColumnView* cv, guint pos, gpointer data) {
    AppState* a = static_cast<AppState*>(data);
    GObject* row = G_OBJECT(g_list_model_get_item(G_LIST_MODEL(gtk_column_view_get_model(cv)), pos));
    ArchNode* n = nodo_de_fila(row);
    Nodo tipo = n ? n->tipo : Nodo::Grupo;
    std::vector<size_t> ruta = n ? n->ruta : std::vector<size_t>{};
    int idx = n ? n->idx : -1;
    if (row) g_object_unref(row);
    switch (tipo) {
        case Nodo::Cuadro: a->rutaSeleccionada = ruta; abrir_dialogo_carga(a); break;          // nueva carga en el cuadro
        case Nodo::Carga: a->rutaSeleccionada = ruta; abrir_dialogo_carga(a, idx); break;      // editar la carga
        case Nodo::Bateria: if (!ruta.empty()) dialogo_bateria(a, int(ruta.front())); break;     // batería del CGBT
        case Nodo::Equipo:
            if (idx >= 0 && size_t(idx) < a->proyecto.equipos.size()) {
                if (es_sai(a->proyecto.equipos[size_t(idx)])) dialogo_sai(a, idx); else dialogo_ge(a, idx);
            }
            break;
        default: break;
    }
}

static GtkWidget* boton_con_icono(const char* icono, const char* texto) {
    GtkWidget* b = gtk_button_new();
    GtkWidget* c = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(c), icono);
    adw_button_content_set_label(ADW_BUTTON_CONTENT(c), texto);
    gtk_button_set_child(GTK_BUTTON(b), c);
    return b;
}

static GtkWidget* construir_cuadros(AppState* a) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget* barra = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(barra, 12); gtk_widget_set_margin_end(barra, 12);
    gtk_widget_set_margin_top(barra, 8); gtk_widget_set_margin_bottom(barra, 8);
    a->archResumen = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a->archResumen), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(a->archResumen), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(a->archResumen, TRUE);
    gtk_widget_add_css_class(a->archResumen, "dim-label");
    GtkWidget* bAdd = boton_con_icono("list-add-symbolic", "Añadir carga…");
    gtk_widget_set_tooltip_text(bAdd, "Añadir una carga al cuadro seleccionado (o doble clic sobre un cuadro)");
    gtk_widget_add_css_class(bAdd, "suggested-action");
    g_signal_connect(bAdd, "clicked", G_CALLBACK(on_add_load), a);
    GtkWidget* bMover = boton_con_icono("go-next-symbolic", "Mover cargas…");
    gtk_widget_set_tooltip_text(bMover, "Mover las cargas seleccionadas a otro cuadro (Ctrl o Mayús + clic para seleccionar varias)");
    conectar_clic(bMover, [a] { mover_cargas(a); });
    gtk_box_append(GTK_BOX(barra), a->archResumen);
    gtk_box_append(GTK_BOX(barra), bMover);
    gtk_box_append(GTK_BOX(barra), bAdd);
    gtk_box_append(GTK_BOX(box), barra);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    a->archView = gtk_column_view_new(nullptr);
    GtkColumnView* cv = GTK_COLUMN_VIEW(a->archView);
    gtk_column_view_set_show_row_separators(cv, TRUE);
    gtk_column_view_set_show_column_separators(cv, TRUE);
    gtk_widget_add_css_class(a->archView, "data-table");
    nueva_columna(cv, "Elemento", C_ELEMENTO, 230);
    nueva_columna(cv, "Rev.", C_REV, 55);
    nueva_columna(cv, "Descripción", C_DESCRIPCION, 190);
    nueva_columna(cv, "Tipo", C_TIPO, 170);
    nueva_columna(cv, "Fases / U", C_FASES, 110);
    nueva_columna(cv, "L [m]", C_LONG, 70);
    nueva_columna(cv, "P inst. [kW]", C_PINST, 95);
    nueva_columna(cv, "Ku", C_KU, 50);
    nueva_columna(cv, "Ks", C_KS, 50);
    nueva_columna(cv, "Ke", C_KE, 50);
    nueva_columna(cv, "P dem. [kW]", C_PDEM, 95);
    nueva_columna(cv, "Q dem. [kVAr]", C_QDEM, 105);
    nueva_columna(cv, "S dem. [kVA]", C_SDEM, 100);
    nueva_columna(cv, "cos φ", C_COS, 70);
    nueva_columna(cv, "Emerg.", C_EMER, 75);
    nueva_columna(cv, "", C_AVISOS, 36);
    g_signal_connect(cv, "activate", G_CALLBACK(on_arch_activate), a);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), a->archView);
    gtk_widget_set_vexpand(scroll, TRUE);

    AdwStatusPage* vacio = ADW_STATUS_PAGE(adw_status_page_new());
    adw_status_page_set_icon_name(vacio, "view-grid-symbolic");
    adw_status_page_set_title(vacio, "Sin cuadros eléctricos");
    adw_status_page_set_description(vacio, "Crea los cuadros en la pestaña «Arquitectura eléctrica» o añade aquí un cuadro general para empezar a conectar cargas.");
    GtkWidget* bCuadro = gtk_button_new_with_label("Añadir cuadro general");
    gtk_widget_add_css_class(bCuadro, "pill");
    gtk_widget_add_css_class(bCuadro, "suggested-action");
    gtk_widget_set_halign(bCuadro, GTK_ALIGN_CENTER);
    g_signal_connect(bCuadro, "clicked", G_CALLBACK(on_add_board), a);
    adw_status_page_set_child(vacio, bCuadro);

    a->archStack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(a->archStack), scroll, "arbol");
    gtk_stack_add_named(GTK_STACK(a->archStack), GTK_WIDGET(vacio), "vacio");
    gtk_widget_set_vexpand(a->archStack, TRUE);
    gtk_box_append(GTK_BOX(box), a->archStack);
    return box;
}

static void activate(GtkApplication* app, gpointer data);

static void on_application_open(GApplication* app, GFile** files, gint n_files,
                                const gchar* hint, gpointer data) {
    (void)hint;
    AppState* a=(AppState*)data;
    if (a->window == nullptr) activate(GTK_APPLICATION(app),data);
    for (gint i=0;i<n_files;i++) {
        char* path=g_file_get_path(files[i]);
        if (path) {
            open_path(a,path);
            g_free(path);
        }
    }
}

// Logo (res/logo.svg, incrustado al compilar): se registra en el tema de iconos con el identificador de la
// aplicación, para el icono de las ventanas y el diálogo «Acerca de».
static const char* ICONO_APP = "com.apespower.electricaltools";   // identificador de la aplicación

// Tema de iconos propio («ElectricalTools»): los iconos de res/iconos (incrustados al compilar) y el logo se
// escriben en la caché del usuario y se activa este tema para la aplicación. Hereda del tema del
// escritorio, así que cualquier icono que no esté en res/iconos se sigue tomando de él.
static void instalar_logo() {
    namespace fs = std::filesystem;
    fs::path base = fs::path(g_get_user_cache_dir()) / "electrical-tools" / "temas";
    fs::path tema = base / "ElectricalTools";
    std::error_code ec;
    fs::create_directories(tema / "scalable" / "actions", ec);
    fs::create_directories(tema / "scalable" / "apps", ec);
    auto escribir = [](const fs::path& f, const char* datos) {
        gchar* actual = nullptr;
        if (!g_file_get_contents(f.c_str(), &actual, nullptr, nullptr) || std::string(actual) != datos)
            g_file_set_contents(f.c_str(), datos, -1, nullptr);
        g_free(actual);
    };
    for (const IconoIncrustado& i: ICONOS)
        escribir(tema / "scalable" / "actions" / (std::string(i.nombre) + ".svg"), reinterpret_cast<const char*>(i.datos));
    escribir(tema / "scalable" / "apps" / (std::string(ICONO_APP) + ".svg"), reinterpret_cast<const char*>(LOGO_SVG));

    GtkSettings* ajustes = gtk_settings_get_default();
    gchar* previo = nullptr;
    g_object_get(ajustes, "gtk-icon-theme-name", &previo, nullptr);
    std::string hereda = (previo && g_strcmp0(previo, "ElectricalTools") != 0) ? std::string(previo) + "," : "";
    g_free(previo);
    std::string indice =
        "[Icon Theme]\nName=ElectricalTools\nComment=Iconos de Electrical Tools\nInherits=" + hereda + "Adwaita,hicolor\n"
        "Directories=scalable/actions,scalable/apps\n\n"
        "[scalable/actions]\nSize=16\nMinSize=8\nMaxSize=512\nType=Scalable\nContext=Actions\n\n"
        "[scalable/apps]\nSize=128\nMinSize=16\nMaxSize=512\nType=Scalable\nContext=Applications\n";
    escribir(tema / "index.theme", indice.c_str());

    gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), base.c_str());
    g_object_set(ajustes, "gtk-icon-theme-name", "ElectricalTools", nullptr);
    gtk_window_set_default_icon_name(ICONO_APP);
}

static void restaurar_estado(AppState* a, const std::string& estado) {
    try {
        a->proyecto = Proyecto::fromJson(json::parse(estado));
    } catch (const std::exception& e) {
        show_error(a, std::string("No se pudo deshacer:\n") + e.what());
        return;
    }
    a->estadoActual = estado;
    a->restaurando = true;
    a->rutaSeleccionada.clear();
    if (a->diagrama) a->diagrama->seleccionar({});
    a->modificado = true;
    refresh_all(a);
    a->restaurando = false;
}

static void deshacer(AppState* a, bool rehacer) {
    // Con un diálogo abierto no se deshace (podría estar editando algo que desaparece)
    if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(a->window))) return;
    auto& origen = rehacer ? a->rehacer : a->deshacer;
    auto& destino = rehacer ? a->deshacer : a->rehacer;
    if (origen.empty()) { show_toast(a->toasts, rehacer ? "No hay nada que rehacer" : "No hay nada que deshacer"); return; }
    destino.push_back(a->estadoActual);
    std::string estado = origen.back();
    origen.pop_back();
    restaurar_estado(a, estado);
    show_toast(a->toasts, rehacer ? "Rehecho" : "Deshecho (Ctrl+Mayús+Z para rehacer)");
}

static void on_exportar(AppState* a, int formato) {
    if (formato == 0) on_export_excel(nullptr, a);
    else if (a->diagrama) a->diagrama->dialogoExportar(formato - 1);
}

static void on_acerca_de(GtkWidget*, gpointer data) {
    AppState* a = static_cast<AppState*>(data);
    AdwDialog* d = adw_about_dialog_new();
    AdwAboutDialog* ad = ADW_ABOUT_DIALOG(d);
    adw_about_dialog_set_application_icon(ad, ICONO_APP);
    adw_about_dialog_set_application_name(ad, "Electrical Tools");
    adw_about_dialog_set_developer_name(ad, "APES Power");
    adw_about_dialog_set_website(ad, "https://github.com/APES-Power/electrical-tools");
    adw_about_dialog_set_issue_url(ad, "https://github.com/APES-Power/electrical-tools/issues");
    adw_about_dialog_add_link(ad, "Código fuente (GitHub)", "https://github.com/APES-Power/electrical-tools");
    adw_about_dialog_set_comments(ad, "Lista de cargas eléctricas de baja tensión, arquitectura de cuadros y fuentes, "
                                      "y exportación a Excel, SVG, DXF y DWG.");
    adw_about_dialog_set_version(ad, "1.0");
    adw_dialog_present(d, a->window);
}

static void activate(GtkApplication* app, gpointer data) {
    AppState* a=(AppState*)data;
    a->app=app;
    g_app=a;
    instalar_logo();

    a->window=adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(a->window),"Electrical Tools");
    gtk_window_set_default_size(GTK_WINDOW(a->window),1280,760);

    AdwToolbarView* toolbar=ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar* header=ADW_HEADER_BAR(adw_header_bar_new());
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(a->window),GTK_WIDGET(toolbar));
    adw_toolbar_view_add_top_bar(toolbar,GTK_WIDGET(header));

    GtkWidget* newButton=gtk_button_new_from_icon_name("document-new-symbolic");
    gtk_widget_set_tooltip_text(newButton,"Nuevo proyecto");
    g_signal_connect(newButton,"clicked",G_CALLBACK(on_new),a);
    adw_header_bar_pack_start(header,newButton);

    GtkWidget* openButton=gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(openButton,"Abrir");
    g_signal_connect(openButton,"clicked",G_CALLBACK(on_open),a);
    adw_header_bar_pack_start(header,openButton);

    GtkWidget* aboutButton=gtk_button_new_from_icon_name("help-about-symbolic");
    gtk_widget_set_tooltip_text(aboutButton,"Acerca de Electrical Tools");
    g_signal_connect(aboutButton,"clicked",G_CALLBACK(on_acerca_de),a);
    adw_header_bar_pack_end(header,aboutButton);

    GtkWidget* saveButton=gtk_button_new_with_label("Guardar");
    gtk_widget_add_css_class(saveButton,"suggested-action");
    g_signal_connect(saveButton,"clicked",G_CALLBACK(on_save),a);
    adw_header_bar_pack_end(header,saveButton);

    GtkWidget* addLoadButton=gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(addLoadButton,"Añadir carga");
    g_signal_connect(addLoadButton,"clicked",G_CALLBACK(on_add_load),a);
    adw_header_bar_pack_end(header,addLoadButton);

    // Un único botón «Exportar»: lista de cargas (Excel) y arquitectura (SVG, DXF, DWG)
    GtkWidget* exportButton=gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(exportButton),"document-save-as-symbolic");
    gtk_widget_set_tooltip_text(exportButton,"Exportar");
    GtkWidget* pop=gtk_popover_new();
    GtkWidget* cajaExp=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
    const char* opciones[4]={"Lista de cargas (Excel)…","Arquitectura eléctrica (SVG)…","Arquitectura eléctrica (DXF)…","Arquitectura eléctrica (DWG)…"};
    const char* iconos[4]={"x-office-spreadsheet-symbolic","image-x-generic-symbolic","x-office-drawing-symbolic","x-office-drawing-symbolic"};
    for(int f=0;f<4;f++){
        GtkWidget* b=boton_con_icono(iconos[f],opciones[f]);
        gtk_widget_add_css_class(b,"flat");
        gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)),GTK_ALIGN_START);
        conectar_clic(b,[a,f,pop]{ gtk_popover_popdown(GTK_POPOVER(pop)); on_exportar(a,f); });
        gtk_box_append(GTK_BOX(cajaExp),b);
    }
    gtk_popover_set_child(GTK_POPOVER(pop),cajaExp);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(exportButton),pop);
    adw_header_bar_pack_end(header,exportButton);

    // Deshacer / rehacer
    GtkEventController* atajos=gtk_shortcut_controller_new();
    struct Atajo{const char* teclas; bool rehacer;};
    for(const Atajo& t:{Atajo{"<Control>z",false},Atajo{"<Control><Shift>z",true},Atajo{"<Control>y",true}}){
        GtkShortcutAction* acc=gtk_callback_action_new(+[](GtkWidget*,GVariant*,gpointer d)->gboolean{
            auto* p=static_cast<std::pair<AppState*,bool>*>(d); deshacer(p->first,p->second); return TRUE; },
            new std::pair<AppState*,bool>(a,t.rehacer),[](gpointer d){ delete static_cast<std::pair<AppState*,bool>*>(d); });
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(atajos),gtk_shortcut_new(gtk_shortcut_trigger_parse_string(t.teclas),acc));
    }
    gtk_widget_add_controller(a->window,atajos);

    AdwViewStack* stack=ADW_VIEW_STACK(adw_view_stack_new());

    AdwViewStackPage* generalStackPage=adw_view_stack_add_titled(stack,construir_general(a),"general","Datos generales");
    adw_view_stack_page_set_icon_name(generalStackPage,"preferences-system-symbolic");

    a->diagrama=new DiagramaCuadros(&a->proyecto, callbacks_diagrama(a));
    AdwViewStackPage* diagStackPage=adw_view_stack_add_titled(stack,a->diagrama->widget(),"arquitectura","Arquitectura eléctrica");
    adw_view_stack_page_set_icon_name(diagStackPage,"network-wired-symbolic");

    AdwViewStackPage* cuadrosStackPage=adw_view_stack_add_titled(stack,construir_cuadros(a),"cuadros","Cuadros y cargas");
    adw_view_stack_page_set_icon_name(cuadrosStackPage,"view-list-symbolic");

    AdwViewSwitcher* switcher=ADW_VIEW_SWITCHER(adw_view_switcher_new());
    adw_view_switcher_set_stack(switcher,stack);
    adw_view_switcher_set_policy(switcher,ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(header,GTK_WIDGET(switcher));

    a->toasts=ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(a->toasts,GTK_WIDGET(stack));
    adw_toolbar_view_set_content(toolbar,GTK_WIDGET(a->toasts));

    refresh_all(a);
    if (!a->rutaInicial.empty()) open_path(a,a->rutaInicial);
    gtk_window_present(GTK_WINDOW(a->window));
}

int main(int argc,char**argv){
    AppState state;
    GtkApplication* app=GTK_APPLICATION(adw_application_new(
        ICONO_APP,G_APPLICATION_HANDLES_OPEN));
    g_set_application_name("Electrical Tools");
    g_signal_connect(app,"activate",G_CALLBACK(activate),&state);
    g_signal_connect(app,"open",G_CALLBACK(on_application_open),&state);
    int status=g_application_run(G_APPLICATION(app),argc,argv);
    g_object_unref(app);return status;
}
