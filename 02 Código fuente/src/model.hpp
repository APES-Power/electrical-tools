#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Portada {
    std::string empresa, codigoDocumento, titulo = "LISTA DE CARGA", escrito, comprobado, verificado;
};

// Transformador (o grupo de transformadores iguales en paralelo: misma potencia y tensión)
struct Transformador {
    std::string tag;
    double sNomKva = 0, sAdmKva = 0, uSecV = 400;   // por unidad
    int nParalelo = 1;
    std::optional<double> diagX, diagY, diagW, diagH;   // posición en el diagrama de arquitectura
    double sNomTotal() const { return sNomKva * std::max(1, nParalelo); }
    double sAdmTotal() const { return sAdmKva * std::max(1, nParalelo); }
};
struct Zona { std::string tag, descripcion; };

// Instalación fotovoltaica conectada a un cuadro de la instalación
struct InstalacionFV {
    std::string rev, tag, nombre;
    double pPicoKwp = 0;            // potencia pico del campo FV (DC)
    double pInversorKw = 0;         // potencia nominal AC de cada inversor
    int nInversores = 1;
    std::string configFases = "3F+N";
    std::string cuadro;             // TAG del cuadro donde se conecta
    std::optional<double> diagX, diagY, diagW, diagH;

    double pAcKw = 0, ratioDcAc = 0;
    std::vector<std::string> avisos;
};

// Tipo de carga (editable en Datos generales)
struct TipoCargaDef {
    std::string nombre;                 // identificador interno (no se muestra), p. ej. «MOTOR»
    std::string prefijo;                // campo <tipo> del TAG, p. ej. «M»
    std::string descripcion;
    std::optional<double> cosFijo;      // vacío: cos φ según la tabla de motores
    int rendimiento = 0;                // 0: no aplica · 1: P absorbida = P/η · 2: η sólo informativo
};
const std::vector<TipoCargaDef>& tiposCargaDeSerie();

struct DatosGenerales {
    bool tagsAutomaticos = true;
    bool tablasAutomaticas = true;
    double pMin1F3FKw = 5.0;
    std::vector<Transformador> transformadores;
    std::vector<Zona> zonas;
    std::vector<InstalacionFV> fotovoltaica;
    std::vector<TipoCargaDef> tiposCarga = tiposCargaDeSerie();
    std::string prefijoBateria = "CAP";               // prefijo del TAG de las baterías de condensadores
    const TipoCargaDef* tipoCarga(const std::string& nombre) const;
    bool usaRendimiento(const std::string& tipo) const;
    std::string descripcionTipo(const std::string& tipo) const;   // lo que se muestra de un tipo de carga
    std::string prefijo(const std::string& tipo) const;   // del TAG (TIPO_BATERIA → prefijoBateria)
    Portada portada;   // se conserva en el fichero, pero se gestiona fuera de la aplicación
    const Transformador* transformador(const std::string& tag) const;
};

struct Carga {
    std::string rev, tag, nombre, tipo;
    double pNomKw = 0;
    std::optional<std::string> configFases;
    std::optional<double> cosPhi;
    std::optional<double> rendimientoPct;
    double ku = 1, ks = 1, ke = 1;
    std::optional<std::string> tagEmer;
    std::optional<double> longitudM;    // longitud del cable hasta el cuadro de alimentación
    std::string zona;                   // vacía = la del cuadro de alimentación
    std::string zonaEfectiva;           // resultado: zona propia o heredada

    std::string fases;
    double tensionV = 0, cosEfectivo = 1, etaPct = 0;
    double nKw = 0, qAbsKvar = 0, pKw = 0, qKvar = 0, sKva = 0, sEmerKva = 0;
    std::vector<std::string> avisos;

    void calcular(const DatosGenerales& dg, const std::optional<std::string>& fasesEquipo);
    json toJson() const;
    static Carga fromJson(const json& j);
};

// Batería de condensadores: sólo en el CGBT. Se dimensiona con la P y Q demandadas del cuadro
// para alcanzar el cos φ objetivo, redondeando al escalón de regulación.
struct Bateria {
    std::string rev, tag, nombre;
    double cosObjetivo = 0.95;
    double escalonKvar = 25;
    std::string configFases = "3F";
    std::optional<double> tensionManual;
    std::optional<double> diagX, diagY, diagW, diagH;   // bloque en el diagrama de arquitectura

    double qKvarNominal = 0;             // resultado: potencia de la batería
    double qNecesariaKvar = 0, cosAntes = 1, cosDespues = 1;
    std::vector<std::string> avisos;
    void calcular(double pKw, double qKvar);

    double tensionV() const;
    double qKvar() const;
    json toJson() const;
    static Bateria fromJson(const json& j);
};

struct Cuadro {
    std::string rev, tag, nombre, tipo = "GENERAL BT", configFases = "3F+N";
    std::string zona, trafo;
    bool calcConTrafo = false;
    bool compensacion = false;  // CGBT con batería de condensadores (se añade como equipo; su tamaño es automático)
    std::optional<std::string> tagEmer;
    std::string saiAlimentacion;   // TAG del SAI del que cuelga el cuadro (vacío = no cuelga de un SAI)
    double ke = 1;
    std::vector<Carga> cargas;
    std::vector<Bateria> baterias;
    std::vector<Cuadro> subcuadros;
    std::optional<double> diagX, diagY, diagW, diagH;   // posición del bloque en el diagrama de arquitectura

    std::string zonaEfectiva, trafoEfectivo;
    double tensionV = 0, pInstKw = 0, pCargasKw = 0, qCargasKvar = 0;
    double pKw = 0, qKvar = 0, sKva = 0, cosPhi = 1, ksGlobal = 1;
    double sEmerKva = 0, sDimensionKva = 0;
    std::vector<std::string> avisos;

    void calcular(const DatosGenerales& dg, const std::string& zonaPadre,
                  const std::string& trafoPadre,
                  const std::unordered_map<std::string, std::string>& fasesEquipos, bool origen = true);
    json toJson() const;
    static Cuadro fromJson(const json& j);
};

struct EquipoEmergencia {
    std::string rev, tag, nombre, tipo;
    bool forzarTrifasico = false;
    double horas = 1, vDepositoInclL = 0, consumoLH = 0, eficienciaPct = 99;
    std::string cuadroAlimentacion;   // SAI: TAG del cuadro que lo alimenta (vacío = sin definir)
    std::optional<double> diagX, diagY, diagW, diagH;

    double sEmerKva = 0, tensionV = 0, sEspMinKva = 0;
    std::optional<double> sDisenoKva, vDepositoAdL, capBateriaKwh;
    std::string fases;
    int nConsumidores = 0;
    std::vector<std::string> avisos;

    void calcular(const DatosGenerales& dg, double sEmer, int n);
    json toJson() const;
    static EquipoEmergencia fromJson(const json& j);
};

// Trazado de una flecha del diagrama editado a mano (como en draw.io). Se identifica por los bloques
// que une. «codo»: coordenada del tramo intermedio (y si la flecha sale en vertical, x si sale en
// horizontal). Lados: 0 arriba, 1 derecha, 2 abajo, 3 izquierda; -1 = automático. «frac»: posición del
// nodo de conexión a lo largo del lado (de izquierda a derecha / de arriba abajo).
struct EstiloArista {
    std::optional<double> codo;
    int ladoOrigen = -1, ladoDestino = -1;
    double fracOrigen = 0.5, fracDestino = 0.5;   // nodo de conexión a lo largo del lado (0,25 / 0,5 / 0,75)
    bool vacio() const { return !codo && ladoOrigen < 0 && ladoDestino < 0 && fracOrigen == 0.5 && fracDestino == 0.5; }
};

struct Proyecto {
    DatosGenerales datos;
    std::vector<Cuadro> cuadros;
    std::vector<EquipoEmergencia> equipos;
    std::vector<std::string> avisos;
    std::map<std::string, EstiloArista> estilosAristas;   // clave: «<tipo>:<tag>><tipo>:<tag>»

    void recalcular();
    json toJson() const;
    static Proyecto fromJson(const json& j);
    void asignarTagsAutomaticos();
    void desasociarEmergencia(const std::string& tag);
    // Cuadro identificado por su ruta de índices (cuadros[i].subcuadros[j]...); nullptr si no existe.
    Cuadro* cuadroPorRuta(const std::vector<size_t>& ruta);
    // Mueve el cuadro «tag» para que cuelgue de «padreTag» (vacío = cuadro de origen). false si no es posible.
    bool moverCuadro(const std::string& tag, const std::string& padreTag);
    // Primer TAG libre para un cuadro del tipo indicado (CGBT, CGBT2, ...)
    std::string tagLibreCuadro(const std::string& tipo) const;
};

// Catálogos para la interfaz
const std::vector<std::string>& configuracionesFases();
extern const char* const TIPO_BATERIA;              // «BAT. COND.»

struct TipoCuadro { std::string tipo, prefijo, descripcion; };
const std::vector<TipoCuadro>& tiposCuadro();
const TipoCuadro& tipoCuadro(const std::string& tipo);   // GENERAL BT si no se encuentra

Proyecto abrirProyecto(const std::string& ruta);
void guardarProyecto(const Proyecto& proyecto, const std::string& ruta);
