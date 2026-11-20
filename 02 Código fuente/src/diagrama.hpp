#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "model.hpp"

// Ruta de índices que identifica un cuadro: cuadros[i].subcuadros[j]...
using RutaCuadro = std::vector<size_t>;

// Tipos de bloque del diagrama: los cuadros y las fuentes que los alimentan
enum class TipoNodo { Cuadro, Trafo, Grupo, FV, SAI, Bateria };   // Bateria: idx = índice del CGBT

struct CallbacksDiagrama {
    std::function<void()> cambioEstructura;             // cuadros creados, conexiones cambiadas...
    std::function<void()> cambioPosicion;               // sólo se han movido bloques
    std::function<void(const RutaCuadro&)> editar;      // doble clic / «Editar» sobre un cuadro
    std::function<void(const RutaCuadro&)> eliminar;    // Supr / «Eliminar» sobre un cuadro
    std::function<void(const RutaCuadro&)> seleccion;   // cuadro seleccionado (ruta vacía = ninguno)
    std::function<void(const std::string&)> aviso;      // mensajes breves para el usuario
    // Fuentes (índice en transformadores / equipos de emergencia / fotovoltaica)
    std::function<void(TipoNodo, size_t)> editarFuente;
    std::function<void(TipoNodo, size_t)> eliminarFuente;
    std::function<void(TipoNodo)> nuevaFuente;
};

// Editor de la arquitectura eléctrica al estilo draw.io. Bloques: cuadros, transformadores, grupos
// electrógenos e instalaciones fotovoltaicas. Flechas: cuadro → subcuadro, trafo → CGBT,
// grupo electrógeno / SAI → cuadro (emergencia), cuadro → SAI (alimentación del SAI) y
// fotovoltaica → cuadro de conexión.
class DiagramaCuadros {
public:
    DiagramaCuadros(Proyecto* proyecto, CallbacksDiagrama callbacks);

    GtkWidget* widget() const { return raiz_; }
    void refrescar();                       // tras cambios en el proyecto hechos desde fuera
    void seleccionar(const RutaCuadro& r);  // seleccionar un cuadro (ruta vacía = nada)
    void encuadrarAlMostrar();              // ajustar la vista al contenido en el próximo dibujado
    void organizar();                       // disposición automática

    // Exportación del diagrama (sin rejilla ni selección). Devuelven false y rellenan «error» si fallan.
    bool exportarSVG(const std::string& ruta, std::string& error);
    bool exportarDXF(const std::string& ruta, std::string& error);
    // DWG: se genera un DXF y se convierte con ODA File Converter o LibreDWG (dxf2dwg) si están instalados.
    // Si no hay conversor, guarda el DXF junto a la ruta pedida y lo explica en «error».
    bool exportarDWG(const std::string& ruta, std::string& error);
    void dialogoExportar(int formato);   // pide el fichero y exporta: 0 SVG, 1 DXF, 2 DWG

private:
    // AlimSAI: cuadro → SAI (entrada del SAI); SalidaSAI: SAI → cuadro que cuelga de él; AlimBat: CGBT → batería
    enum class TipoArista { Alimentacion, Trafo, Emergencia, FV, SAI, AlimSAI, AlimBat, SalidaSAI };
    enum class Modo { Nada, Mover, Pan, Conectar, Extremo, Codo, Redimensionar };
    enum class Sel { Nada, Nodo, Arista };

    struct Clave {
        TipoNodo tipo = TipoNodo::Cuadro;
        RutaCuadro ruta;     // cuadros
        std::string tag;     // fuentes
        bool operator==(const Clave& o) const { return tipo == o.tipo && ruta == o.ruta && tag == o.tag; }
    };
    struct Nodo {
        TipoNodo tipo = TipoNodo::Cuadro;
        RutaCuadro ruta;
        Cuadro* c = nullptr;
        size_t idx = 0;                       // índice de la fuente en su lista del proyecto
        std::string tag;
        std::optional<double>* x = nullptr;   // posición en el modelo
        std::optional<double>* y = nullptr;
        std::optional<double>* w = nullptr;   // tamaño (vacío = el de serie)
        std::optional<double>* h = nullptr;
        int padre = -1;                       // cuadro aguas arriba (sólo cuadros)
    };
    struct Arista { int desde = -1, hasta = -1; TipoArista tipo = TipoArista::Alimentacion; };

    Proyecto* p_;
    CallbacksDiagrama cb_;
    GtkWidget *raiz_ = nullptr, *area_ = nullptr, *zoomLabel_ = nullptr;
    GtkWidget *btnSub_ = nullptr, *btnEditar_ = nullptr, *btnEliminar_ = nullptr, *popFuentes_ = nullptr;
    GtkWidget* btnTrazado_ = nullptr;
    GtkWidget* popExportar_ = nullptr;
    bool exportando_ = false;               // dibujo para exportar: sin rejilla, selección ni asas
    // Redimensionado: asa arrastrada (0 NO, 1 N, 2 NE, 3 E, 4 SE, 5 S, 6 SO, 7 O) y rectángulo inicial
    int dragResize_ = -1;
    double rx0_ = 0, ry0_ = 0, rw0_ = 0, rh0_ = 0;

    std::vector<Nodo> nodos_;
    std::vector<Arista> aristas_;
    double ox_ = 40, oy_ = 40, zoom_ = 1.0;
    Sel sel_ = Sel::Nada;
    Clave selA_, selB_;                       // nodo seleccionado, o extremos de la flecha seleccionada
    TipoArista selTipoArista_ = TipoArista::Alimentacion;
    int hover_ = -1, hoverHandle_ = -1, handleOwner_ = -1;
    bool encuadrar_ = true;
    std::optional<Clave> porMostrar_;         // bloque recién creado que debe quedar a la vista

    Modo modo_ = Modo::Nada;
    int dragNodo_ = -1, dragDir_ = -1;
    bool movido_ = false;
    double startSX_ = 0, startSY_ = 0, nodoX0_ = 0, nodoY0_ = 0, panX0_ = 0, panY0_ = 0;
    double curWX_ = 0, curWY_ = 0, lastSX_ = 0, lastSY_ = 0;
    // Edición de flechas: extremo arrastrado (0 = origen, 1 = destino) y clave del trazado editado
    int dragArista_ = -1, dragExtremo_ = -1;
    std::string dragEstilo_;
    bool dragEstiloNuevo_ = false;
    // Ajuste al centro (como draw.io): guías de alineación al mover y punto de conexión al arrastrar flechas
    std::optional<double> guiaX_, guiaY_;
    int destinoSnap_ = -1, puntoSnap_ = -2;
    int nodoOrigen_ = -1;
    int hoverNodo_ = -1;                      // nodo de conexión bajo el ratón                     // nodo de conexión desde el que se arrastra una flecha nueva (-1: flecha azul)
    int hitNodoConexion(int nodo, double wx, double wy) const;   // bloque bajo el ratón y punto: -1 centro, 0..3 lado, -2 ninguno

    // modelo
    void reconstruir();
    Clave claveDe(int i) const;
    int indice(const Clave& k) const;
    int indiceCuadro(const RutaCuadro& r) const;
    int indiceArista() const;                 // flecha seleccionada en aristas_, o -1
    double X(int i) const { return **nodos_[size_t(i)].x; }
    double Y(int i) const { return **nodos_[size_t(i)].y; }
    double Wn(int i) const;
    double Hn(int i) const;
    bool tienePos(int i) const { return nodos_[size_t(i)].x->has_value() && nodos_[size_t(i)].y->has_value(); }
    RutaCuadro crearCuadro(const RutaCuadro& padre, double x, double y);
    bool reconectar(const RutaCuadro& hijo, const RutaCuadro& nuevoPadre, RutaCuadro& nuevaRuta);
    bool valida(int origen, int destino, std::string* motivo) const;
    bool conectar(int origen, int destino);
    bool eliminarArista(int ia, bool silencioso);
    void reconectarArista(int ia, int extremo, int nuevoNodo);
    int indiceTag(TipoNodo t, const std::string& tag) const;
    std::string claveEstilo(const Arista& a) const;
    const EstiloArista* estilo(const Arista& a) const;
    void posicionLibre(double& x, double& y, int dir, int ignorar = -1) const;
    void colocarFuentesSinPosicion();

    // geometría
    void aMundo(double sx, double sy, double& wx, double& wy) const;
    int hitNodo(double wx, double wy) const;
    int hitHandle(int nodo, double wx, double wy) const;
    int hitArista(double wx, double wy) const;
    void handle(int nodo, int dir, double& x, double& y) const;
    std::vector<std::pair<double, double>> recorrido(const Arista& a) const;
    std::vector<std::pair<double, double>> recorridoAuto(const Arista& a) const;
    std::vector<std::pair<double, double>> recorridoEstilo(const Arista& a, const EstiloArista& e) const;
    void puntoLado(int nodo, int lado, double frac, double& x, double& y) const;
    int ladoMasCercano(int nodo, double wx, double wy) const;
    int puntoConexion(int nodo, double wx, double wy, double& px, double& py) const;
    void actualizarSnapConexion();
    void fijarLado(TipoNodo to, const std::string& tagO, TipoNodo td, const std::string& tagD, int extremo, int lado);
    static std::string claveEstiloTags(TipoNodo to, const std::string& tagO, TipoNodo td, const std::string& tagD);
    void puntoResize(int nodo, int k, double& x, double& y) const;
    int hitResize(int nodo, double wx, double wy) const;
    void extension(double& x0, double& y0, double& x1, double& y1) const;
    void ajustar();
    void asegurarVisible(int nodo, int w, int h);
    void setZoom(double z, double sx, double sy);

    // interfaz
    void dibujar(cairo_t* cr, int w, int h);
    void dibujarBloque(cairo_t* cr, int i);
    void datosBloque(int i, std::string& l1, std::string& l2, std::string& l3, int& estilo, bool& avisos) const;
    void selNodo(int i);
    void selArista(int a);
    void limpiarSel();
    void notificarSeleccion();
    void actualizarBotones();
    void nuevoCuadroGeneral();
    void anadirSubcuadro();
    void editarSeleccion();
    void eliminarSeleccion();

    static void onDraw(GtkDrawingArea*, cairo_t*, int, int, gpointer);
    static void onDragBegin(GtkGestureDrag*, double, double, gpointer);
    static void onDragUpdate(GtkGestureDrag*, double, double, gpointer);
    static void onDragEnd(GtkGestureDrag*, double, double, gpointer);
    static void onPressed(GtkGestureClick*, int, double, double, gpointer);
    static void onMotion(GtkEventControllerMotion*, double, double, gpointer);
    static void onLeave(GtkEventControllerMotion*, gpointer);
    static gboolean onScroll(GtkEventControllerScroll*, double, double, gpointer);
    static gboolean onKey(GtkEventControllerKey*, guint, guint, GdkModifierType, gpointer);
};
