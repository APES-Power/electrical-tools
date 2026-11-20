#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace {
constexpr double EPS = 1e-9;


const std::vector<double> motorP = {0.37,0.55,0.75,1.1,1.5,2.2,3,4,5.5,7.5,11,15,18.5,22,30,37,45,55,75,90,110,132,160,200,250,315,355,400,450,500,560,630,710};
const std::vector<double> motorCos = {0.75,0.73,0.76,0.78,0.78,0.80,0.82,0.81,0.84,0.85,0.81,0.84,0.84,0.83,0.83,0.84,0.83,0.86,0.86,0.87,0.87,0.87,0.86,0.86,0.87,0.87,0.87,0.87,0.87,0.88,0.90,0.87,0.87};
const std::vector<double> motorEta = {0.708,0.75,0.763,0.785,0.805,0.825,0.845,0.86,0.874,0.89,0.903,0.911,0.923,0.924,0.929,0.936,0.942,0.946,0.95,0.952,0.956,0.958,0.96,0.962,0.965,0.967,0.967,0.968,0.968,0.968,0.969,0.969,0.969};

const std::vector<double> generadores = {4.8,6,8.2,10,11,14,20,25,33,34,41,48,62,68,78,93,110,135,145,170,200,225,275,300,330,355,400,450,500,550,600,630,660,700,730,800,880,915,1000,1100,1375,1650,1875,1925,2000,2200};
const std::vector<double> sai1f = {0.7,1,1.5,2,3,4,5,6,8,10,12,15,20};
const std::vector<double> sai3f = {10,15,20,25,30,40,50,60,80,100,120,160,200,250,300,400,500};

double interpolar(double x, const std::vector<double>& xs, const std::vector<double>& ys) {
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back()) return ys.back();
    auto it = std::lower_bound(xs.begin(), xs.end(), x);
    size_t i = std::distance(xs.begin(), it);
    if (std::abs(xs[i] - x) < EPS) return ys[i];
    double x0=xs[i-1], x1=xs[i], y0=ys[i-1], y1=ys[i];
    return y0 + (y1-y0)*(x-x0)/(x1-x0);
}

std::optional<double> siguienteValor(double x, const std::vector<double>& tabla) {
    for (double v: tabla) if (v >= x-EPS) return v;
    return std::nullopt;
}

double tensionPorFases(const std::string& f) { return f == "1F+N" ? 230.0 : 400.0; }

void visitaCuadros(std::vector<Cuadro>& cuadros, const std::function<void(Cuadro&)>& fn) {
    for (Cuadro& c: cuadros) {
        fn(c);
        visitaCuadros(c.subcuadros, fn);
    }
}
}

const char* const TIPO_BATERIA = "BAT. COND.";

// Posición y tamaño del bloque en el diagrama de arquitectura: "diagrama": {x, y, w, h}
static void guardarPos(json& j, const std::optional<double>& x, const std::optional<double>& y,
                       const std::optional<double>& w, const std::optional<double>& h) {
    if (!(x && y)) return;
    j["diagrama"] = {{"x", *x}, {"y", *y}};
    if (w && h) { j["diagrama"]["w"] = *w; j["diagrama"]["h"] = *h; }
}
static void leerPos(const json& j, std::optional<double>& x, std::optional<double>& y,
                    std::optional<double>& w, std::optional<double>& h) {
    if (!j.contains("diagrama") || !j["diagrama"].is_object()) return;
    const json& d = j["diagrama"];
    if (d.contains("x") && d["x"].is_number() && d.contains("y") && d["y"].is_number()) { x = d["x"].get<double>(); y = d["y"].get<double>(); }
    if (d.contains("w") && d["w"].is_number() && d.contains("h") && d["h"].is_number()) { w = d["w"].get<double>(); h = d["h"].get<double>(); }
}

const std::vector<TipoCargaDef>& tiposCargaDeSerie() {
    static const std::vector<TipoCargaDef> tipos = {
        {"AL. EMER.", "AEM", "ALUMBRADO DE EMERGENCIA", 0.95, 0},
        {"ALUMB.", "AL", "ALUMBRADO", 0.95, 0},
        {"CCTV", "CCTV", "CÁMARAS DE VIDEOVIGILANCIA", 0.90, 0},
        {"CPD", "CPD", "CENTRO DE PROCESAMIENTO DE DATOS", 0.90, 0},
        {"FOTOVOLT.", "PV", "GENERADOR FOTOVOLTAICO", 1.00, 0},
        {"FUERZA", "TF", "TOMA DE FUERZA", 0.85, 0},
        {"GENERAL", "GEN", "GENERAL", 0.85, 0},
        {"GENSET", "G", "GENERADOR DE APOYO", 0.80, 0},
        {"HVAC", "HVAC", "CLIMATIZACIÓN GENERAL (HVAC)", std::nullopt, 1},
        {"I&C", "IC", "INSTRUMENTACIÓN Y CONTROL", 0.90, 0},
        {"M+VARIADOR", "VAR", "MOTOR CON VARIADOR DE FRECUENCIA", 0.95, 2},
        {"MOTOR", "M", "MOTOR", std::nullopt, 1},
        {"PUESTO OF.", "PT", "PUESTOS DE TRABAJO OFICINAS", 0.85, 0},
        {"RECARGA VE", "VE", "PUNTO DE RECARGA VEHÍCULO ELÉCTRICO", 1.00, 0},
        {"RESERVA", "RES", "RESERVA", 1.00, 0},
        {"SPLIT", "SPL", "SPLIT", 0.80, 0},
        {"SS.AA.", "SSAA", "SERVICIOS AUXILIARES", 0.85, 0},
        {"SUM. COMP.", "SUM", "SUMINISTRO COMPLEMENTARIO", 0.80, 0},
    };
    return tipos;
}

const TipoCargaDef* DatosGenerales::tipoCarga(const std::string& nombre) const {
    for (const TipoCargaDef& t: tiposCarga) if (t.nombre == nombre) return &t;
    return nullptr;
}

std::string DatosGenerales::descripcionTipo(const std::string& tipo) const {
    if (tipo == TIPO_BATERIA) return "BATERÍA DE CONDENSADORES";
    const TipoCargaDef* t = tipoCarga(tipo);
    return (t && !t->descripcion.empty()) ? t->descripcion : tipo;
}

bool DatosGenerales::usaRendimiento(const std::string& tipo) const {
    const TipoCargaDef* t = tipoCarga(tipo);
    return t && t->rendimiento != 0;
}

std::string DatosGenerales::prefijo(const std::string& tipo) const {
    if (tipo == TIPO_BATERIA) return prefijoBateria.empty() ? "CAP" : prefijoBateria;
    const TipoCargaDef* t = tipoCarga(tipo);
    return (t && !t->prefijo.empty()) ? t->prefijo : "GEN";
}

const Transformador* DatosGenerales::transformador(const std::string& tag) const {
    for (const auto& t: transformadores) if (t.tag == tag) return &t;
    return nullptr;
}

void Carga::calcular(const DatosGenerales& dg, const std::optional<std::string>& fasesEquipo) {
    avisos.clear(); nKw=qAbsKvar=pKw=qKvar=sKva=sEmerKva=0; etaPct=0;
    fases = configFases.value_or(fasesEquipo.value_or("3F+N"));
    tensionV = tensionPorFases(fases);
    if (fases == "1F+N" && pNomKw >= dg.pMin1F3FKw-EPS)
        avisos.push_back("carga monofásica con P >= " + std::to_string(dg.pMin1F3FKw));

    const TipoCargaDef* def = dg.tipoCarga(tipo);
    std::optional<double> fijo = def ? def->cosFijo : std::nullopt;
    int modoEta = def ? def->rendimiento : 0;
    double cos = cosPhi.value_or(0);
    if (!cosPhi) {
        if (dg.tablasAutomaticas) cos = fijo.value_or(interpolar(pNomKw,motorP,motorCos));
        else { cos=1; avisos.push_back("falta cos φ; tablas desactivadas, se usa 1,0"); }
    }
    cos = std::clamp(cos,1e-6,1.0); cosEfectivo=cos;

    if (modoEta != 0) {
        if (rendimientoPct) etaPct=*rendimientoPct;
        else if (dg.tablasAutomaticas) etaPct=interpolar(pNomKw,motorP,motorEta)*100.0;
        else if (modoEta == 1) { etaPct=100; avisos.push_back("falta η; se usa 100 %"); }
    }
    nKw = (modoEta == 1 && etaPct > EPS) ? pNomKw/(etaPct/100.0) : pNomKw;   // 1: P absorbida = P/η
    qAbsKvar=nKw*std::tan(std::acos(cos));
    double f=ku*ks;
    pKw=nKw*f; qKvar=qAbsKvar*f; sKva=std::hypot(pKw,qKvar); sEmerKva=sKva*ke;
}

double Bateria::tensionV() const {
    if (tensionManual) return *tensionManual;
    return tensionPorFases(configFases);
}
double Bateria::qKvar() const { return -std::abs(qKvarNominal); }

void Bateria::calcular(double p, double q) {
    avisos.clear();
    double s = std::hypot(p, q);
    cosAntes = s > EPS ? p / s : 1;
    double objetivo = std::clamp(cosObjetivo, 0.5, 1.0);
    qNecesariaKvar = q - p * std::tan(std::acos(objetivo));
    if (qNecesariaKvar <= EPS) {
        qNecesariaKvar = 0; qKvarNominal = 0;   // el cos φ ya alcanza el objetivo
    } else {
        qKvarNominal = escalonKvar > EPS ? std::ceil(qNecesariaKvar / escalonKvar - 1e-9) * escalonKvar : qNecesariaKvar;
    }
    double s2 = std::hypot(p, q - qKvarNominal);
    cosDespues = s2 > EPS ? p / s2 : 1;
}

void Cuadro::calcular(const DatosGenerales& dg, const std::string& zonaPadre,
                      const std::string& trafoPadre,
                      const std::unordered_map<std::string,std::string>& fasesEquipos, bool origen) {
    avisos.clear(); zonaEfectiva=zona.empty()?zonaPadre:zona; trafoEfectivo=trafo.empty()?trafoPadre:trafo;
    const Transformador* t=dg.transformador(trafoEfectivo);
    tensionV=(t && configFases != "1F+N") ? t->uSecV : tensionPorFases(configFases);
    for (Carga& c: cargas) {
        std::optional<std::string> fe;
        if (c.tagEmer) { auto it=fasesEquipos.find(*c.tagEmer); if(it!=fasesEquipos.end()) fe=it->second; }
        c.calcular(dg,fe);
        c.zonaEfectiva=c.zona.empty()?zonaEfectiva:c.zona;
    }
    for (Cuadro& s: subcuadros) s.calcular(dg,zonaEfectiva,trafoEfectivo,fasesEquipos,false);

    pInstKw=pCargasKw=qCargasKvar=pKw=qKvar=sKva=sEmerKva=0;
    for(const Carga& c:cargas){pInstKw+=c.pNomKw;pCargasKw+=c.nKw;qCargasKvar+=c.qAbsKvar;pKw+=c.pKw;qKvar+=c.qKvar;sEmerKva+=c.sEmerKva;}
    for(const Cuadro& s:subcuadros){pInstKw+=s.pInstKw;pKw+=s.pKw;qKvar+=s.qKvar;sEmerKva+=s.sEmerKva;}
    for(Bateria& b:baterias){
        if(!origen){b.avisos.assign(1,"la batería de condensadores sólo puede instalarse en el CGBT");b.qKvarNominal=0;continue;}
        b.calcular(pKw,qKvar); qKvar+=b.qKvar();
    }
    sKva=std::hypot(pKw,qKvar); cosPhi=sKva>EPS?pKw/sKva:1;
    double denom=pCargasKw;
    for(const Cuadro& s:subcuadros) denom+=s.pKw;
    ksGlobal=denom>EPS?pKw/denom:1;
    sEmerKva*=ke; sDimensionKva=(calcConTrafo&&t)?t->sAdmTotal():sKva;
    if(cosPhi<0.9&&sKva>EPS) avisos.push_back("cos φ bajo; revisar compensación");
}

void EquipoEmergencia::calcular(const DatosGenerales& dg,double sEmer,int n) {
    avisos.clear(); sEmerKva=sEmer;nConsumidores=n;
    bool tres=(sEmer>=dg.pMin1F3FKw-EPS)||forzarTrifasico;
    fases=tres?"3F+N":"1F+N"; tensionV=tensionPorFases(fases);
    sEspMinKva=sEmer*1.3;
    if(tipo=="G. ELEC.") {
        sDisenoKva=siguienteValor(sEmer,generadores);
        vDepositoAdL=std::max(0.0,consumoLH*horas-vDepositoInclL)*1.05;
    } else {
        sDisenoKva=siguienteValor(sEmer,fases=="1F+N"?sai1f:sai3f);
        capBateriaKwh=eficienciaPct>0?sEmer*horas/(eficienciaPct/100.0):0;
    }
    if(sEmer>0&&!sDisenoKva) avisos.push_back("S de emergencia por encima de la tabla");
    if(n==0) avisos.push_back("ninguna carga ni cuadro tiene asociado este equipo");
}

void Proyecto::asignarTagsAutomaticos() {
    std::unordered_map<std::string,int> cont;
    auto sig=[&](const std::string& zona,const std::string& pref){
        std::string key=zona+"|"+pref; int n=++cont[key];
        char num[16]; std::snprintf(num,sizeof num,"%02d",n);   // <zona>-<tipo>-<id de 2 dígitos>
        return (zona.empty()?"":zona+"-")+pref+"-"+num;
    };
    auto visita=[&](const auto& self,Cuadro& c,const std::string& zonaPadre)->void{
        std::string zona=c.zona.empty()?zonaPadre:c.zona;
        for(Carga& carga:c.cargas){carga.tag=sig(carga.zona.empty()?zona:carga.zona,datos.prefijo(carga.tipo));}
        for(Bateria& b:c.baterias)b.tag=sig(zona,datos.prefijo(TIPO_BATERIA));
        for(Cuadro& s:c.subcuadros)self(self,s,zona);
    };
    for(Cuadro& c:cuadros) visita(visita,c,"");
}

static void recorrerCuadros(const std::vector<Cuadro>& cs,const std::function<void(const Cuadro&)>& fn){for(const Cuadro& c:cs){fn(c);recorrerCuadros(c.subcuadros,fn);}}

static Cuadro* buscarCuadro(std::vector<Cuadro>& cs, const std::string& tag) {
    for (Cuadro& c: cs) {
        if (c.tag == tag) return &c;
        if (Cuadro* r = buscarCuadro(c.subcuadros, tag)) return r;
    }
    return nullptr;
}

// Cuadro padre (tag) del cuadro «tag»; "" si es de origen; std::nullopt si no existe
static std::optional<std::string> padreDeCuadro(const std::vector<Cuadro>& cs, const std::string& tag, const std::string& padre) {
    for (const Cuadro& c: cs) {
        if (c.tag == tag) return padre;
        if (auto r = padreDeCuadro(c.subcuadros, tag, c.tag)) return r;
    }
    return std::nullopt;
}

static bool extraerCuadro(std::vector<Cuadro>& cs, const std::string& tag, Cuadro& fuera) {
    for (size_t i = 0; i < cs.size(); i++) {
        if (cs[i].tag == tag) { fuera = std::move(cs[i]); cs.erase(cs.begin() + long(i)); return true; }
        if (extraerCuadro(cs[i].subcuadros, tag, fuera)) return true;
    }
    return false;
}

bool Proyecto::moverCuadro(const std::string& tag, const std::string& padreTag) {
    Cuadro* c = buscarCuadro(cuadros, tag);
    if (!c) return false;
    if (!padreTag.empty() && (padreTag == tag || buscarCuadro(c->subcuadros, padreTag))) return false;   // ciclo
    if (!padreTag.empty() && !buscarCuadro(cuadros, padreTag)) return false;
    Cuadro mov;
    if (!extraerCuadro(cuadros, tag, mov)) return false;
    if (padreTag.empty()) { cuadros.push_back(std::move(mov)); return true; }
    mov.trafo.clear();                    // un subcuadro no tiene transformador ni batería propios
    mov.baterias.clear(); mov.compensacion = false;
    buscarCuadro(cuadros, padreTag)->subcuadros.push_back(std::move(mov));
    return true;
}

void Proyecto::recalcular() {
    // Cuadros que cuelgan de un SAI: van debajo del cuadro que alimenta al SAI (así su potencia llega aguas
    // arriba) o como cuadros de origen si el SAI no tiene cuadro de alimentación. Si el SAI no existe, se sueltan.
    {
        std::vector<std::pair<std::string, std::string>> mover;   // cuadro, nuevo padre
        std::function<void(std::vector<Cuadro>&)> v = [&](std::vector<Cuadro>& cs) {
            for (Cuadro& c: cs) {
                if (!c.saiAlimentacion.empty()) {
                    const EquipoEmergencia* sai = nullptr;
                    for (const EquipoEmergencia& e: equipos) if (e.tag == c.saiAlimentacion && e.tipo == "UPS") sai = &e;
                    if (!sai) c.saiAlimentacion.clear();
                    else {
                        c.tagEmer = sai->tag;
                        auto actual = padreDeCuadro(cuadros, c.tag, "");
                        if (actual && *actual != sai->cuadroAlimentacion) mover.push_back({c.tag, sai->cuadroAlimentacion});
                    }
                }
                v(c.subcuadros);
            }
        };
        v(cuadros);
        for (auto& [t, padre]: mover)
            if (!moverCuadro(t, padre))
                if (Cuadro* c = buscarCuadro(cuadros, t)) c->saiAlimentacion.clear();   // imposible (ciclo): se suelta del SAI
    }

    // Batería de condensadores automática en cada CGBT (cuadro de origen) con compensación activada
    for(Cuadro& c:cuadros){
        if(c.compensacion&&c.baterias.empty()){Bateria b;b.nombre="BATERÍA DE CONDENSADORES";c.baterias.push_back(b);}
        if(!c.compensacion)c.baterias.clear();
    }
    if(datos.tagsAutomaticos) asignarTagsAutomaticos();
    std::unordered_map<std::string,std::string> fases;
    for(int pasada=0;pasada<2;pasada++){
        for(Cuadro& c:cuadros)c.calcular(datos,"","",fases);
        for(EquipoEmergencia& e:equipos){
            double s=0;int n=0;
            std::function<void(const std::vector<Cuadro>&)> visita=[&](const std::vector<Cuadro>& cs){
                for(const Cuadro& c:cs){
                    if(c.tagEmer&&*c.tagEmer==e.tag){s+=c.sEmerKva;n++;continue;}
                    for(const Carga& carga:c.cargas)if(carga.tagEmer&&*carga.tagEmer==e.tag){s+=carga.sEmerKva;n++;}
                    visita(c.subcuadros);
                }
            }; visita(cuadros); e.calcular(datos,s,n);
        }
        fases.clear();for(const EquipoEmergencia& e:equipos)fases[e.tag]=e.fases;
    }
    // SAI: el cuadro de alimentación debe existir
    for(EquipoEmergencia& e:equipos){
        if(e.cuadroAlimentacion.empty()) continue;
        bool existe=false;
        recorrerCuadros(cuadros,[&](const Cuadro& c){if(c.tag==e.cuadroAlimentacion)existe=true;});
        if(!existe) e.avisos.push_back("el cuadro de alimentación «"+e.cuadroAlimentacion+"» no existe");
    }

    // Fotovoltaica
    for(InstalacionFV& f:datos.fotovoltaica){
        f.avisos.clear();
        f.pAcKw=std::max(0,f.nInversores)*f.pInversorKw;
        f.ratioDcAc=f.pAcKw>EPS?f.pPicoKwp/f.pAcKw:0;
        if(f.pAcKw<=EPS) f.avisos.push_back("falta la potencia de los inversores");
        else if(f.ratioDcAc>1.3+EPS) f.avisos.push_back("ratio DC/AC > 1,3: campo FV sobredimensionado respecto a los inversores");
        else if(f.ratioDcAc<0.8-EPS) f.avisos.push_back("ratio DC/AC < 0,8: inversores sobredimensionados");
        if(f.configFases=="1F+N"&&f.pAcKw>=datos.pMin1F3FKw-EPS) f.avisos.push_back("instalación monofásica con P AC >= "+std::to_string(datos.pMin1F3FKw).substr(0,4)+" kW");
        if(f.cuadro.empty()) f.avisos.push_back("sin cuadro de conexión");
        else {
            const Cuadro* destino=nullptr;
            recorrerCuadros(cuadros,[&](const Cuadro& c){if(c.tag==f.cuadro)destino=&c;});
            if(!destino) f.avisos.push_back("el cuadro de conexión «"+f.cuadro+"» no existe");
            else if(const Transformador* t=datos.transformador(destino->trafoEfectivo))
                if(t->sAdmTotal()>EPS&&f.pAcKw>t->sAdmTotal()+EPS) f.avisos.push_back("P AC superior a la S admisible del transformador "+t->tag);
        }
    }

    avisos.clear();
    recorrerCuadros(cuadros,[&](const Cuadro& c){
        for(const std::string& a:c.avisos)avisos.push_back("Cuadro "+c.tag+": "+a);
        for(const Carga& carga:c.cargas)for(const std::string& a:carga.avisos)avisos.push_back("Carga "+carga.tag+": "+a);
    });
    for(const EquipoEmergencia& e:equipos)for(const std::string& a:e.avisos)avisos.push_back("Equipo "+e.tag+": "+a);
    for(const InstalacionFV& f:datos.fotovoltaica)for(const std::string& a:f.avisos)avisos.push_back("Fotovoltaica "+f.tag+": "+a);
    recorrerCuadros(cuadros,[&](const Cuadro& c){for(const Bateria& b:c.baterias)for(const std::string& a:b.avisos)avisos.push_back("Batería "+b.tag+": "+a);});
}

Cuadro* Proyecto::cuadroPorRuta(const std::vector<size_t>& ruta) {
    std::vector<Cuadro>* nivel=&cuadros; Cuadro* c=nullptr;
    for(size_t i:ruta){ if(i>=nivel->size()) return nullptr; c=&(*nivel)[i]; nivel=&c->subcuadros; }
    return c;
}


const std::vector<std::string>& configuracionesFases() {
    static const std::vector<std::string> fases = {"1F+N","3F","3F+N"};
    return fases;
}


const std::vector<TipoCuadro>& tiposCuadro() {
    static const std::vector<TipoCuadro> tipos = {
        {"GENERAL BT","CGBT","CUADRO GENERAL DE BAJA TENSIÓN"},
        {"GEN. DIST.","CGD","CUADRO GENERAL DE DISTRIBUCIÓN"},
        {"DIST. SEC.","CDS","CUADRO DE DISTRIBUCIÓN SECUNDARIO"},
        {"PROT. MED.","CPM","CUADRO DE PROTECCIÓN Y MEDIDA"},
        {"ALUM.","CA","CUADRO DE ALUMBRADO"},
        {"FUERZA","CF","CUADRO DE FUERZA"},
        {"F+AL","CFA","CUADRO DE FUERZA Y ALUMBRADO"},
    };
    return tipos;
}

const TipoCuadro& tipoCuadro(const std::string& tipo) {
    for(const TipoCuadro& t:tiposCuadro()) if(t.tipo==tipo) return t;
    return tiposCuadro().front();
}

std::string Proyecto::tagLibreCuadro(const std::string& tipo) const {
    std::unordered_map<std::string,bool> usados;
    recorrerCuadros(cuadros,[&](const Cuadro& c){usados[c.tag]=true;});
    std::string pref=tipoCuadro(tipo).prefijo, cand=pref;
    for(int n=2;usados.count(cand);n++) cand=pref+std::to_string(n);
    return cand;
}

void Proyecto::desasociarEmergencia(const std::string& tag) {
    visitaCuadros(cuadros,[&](Cuadro& c){if(c.saiAlimentacion==tag)c.saiAlimentacion.clear();if(c.tagEmer&&*c.tagEmer==tag)c.tagEmer.reset();for(Carga& carga:c.cargas)if(carga.tagEmer&&*carga.tagEmer==tag)carga.tagEmer.reset();});
}

json Carga::toJson() const {
    json j=json::object();
    j["rev"]=rev; j["tag"]=tag; j["nombre"]=nombre; j["tipo"]=tipo; j["p_nom_kw"]=pNomKw;
    j["config_fases"] = configFases ? json(*configFases) : json(nullptr);
    j["cos_phi"] = cosPhi ? json(*cosPhi) : json(nullptr);
    j["rendimiento_pct"] = rendimientoPct ? json(*rendimientoPct) : json(nullptr);
    j["ku"]=ku; j["ks"]=ks; j["ke"]=ke;
    j["tag_emer"] = tagEmer ? json(*tagEmer) : json(nullptr);
    j["longitud_m"] = longitudM ? json(*longitudM) : json(nullptr);
    j["zona"] = zona;
    return j;
}
Carga Carga::fromJson(const json& j){Carga c;c.rev=j.value("rev","");c.tag=j.value("tag","");c.nombre=j.value("nombre","");c.tipo=j.value("tipo","GENERAL");c.pNomKw=j.value("p_nom_kw",0.0);if(j.contains("config_fases")&&!j["config_fases"].is_null())c.configFases=j["config_fases"].get<std::string>();if(j.contains("cos_phi")&&!j["cos_phi"].is_null())c.cosPhi=j["cos_phi"].get<double>();if(j.contains("rendimiento_pct")&&!j["rendimiento_pct"].is_null())c.rendimientoPct=j["rendimiento_pct"].get<double>();c.ku=j.value("ku",1.0);c.ks=j.value("ks",1.0);c.ke=j.value("ke",1.0);if(j.contains("tag_emer")&&!j["tag_emer"].is_null())c.tagEmer=j["tag_emer"].get<std::string>();if(j.contains("longitud_m")&&j["longitud_m"].is_number())c.longitudM=j["longitud_m"].get<double>();if(j.contains("zona")&&j["zona"].is_string())c.zona=j["zona"].get<std::string>();return c;}

json Bateria::toJson() const {
    json j=json::object(); j["rev"]=rev; j["tag"]=tag; j["nombre"]=nombre;
    j["q_kvar"]=qKvarNominal; j["config_fases"]=configFases;
    j["cos_objetivo"]=cosObjetivo; j["escalon_kvar"]=escalonKvar;
    guardarPos(j,diagX,diagY,diagW,diagH);
    j["tension_v"] = tensionManual ? json(*tensionManual) : json(nullptr);
    return j;
}
Bateria Bateria::fromJson(const json& j){Bateria b;b.rev=j.value("rev","");b.tag=j.value("tag","");b.nombre=j.value("nombre","");b.qKvarNominal=j.value("q_kvar",0.0);b.configFases=j.value("config_fases","3F");b.cosObjetivo=j.value("cos_objetivo",0.95);b.escalonKvar=j.value("escalon_kvar",25.0);leerPos(j,b.diagX,b.diagY,b.diagW,b.diagH);if(j.contains("tension_v")&&!j["tension_v"].is_null())b.tensionManual=j["tension_v"].get<double>();return b;}

json Cuadro::toJson() const {
    json j=json::object();
    j["rev"]=rev; j["tag"]=tag; j["nombre"]=nombre; j["tipo"]=tipo; j["config_fases"]=configFases;
    j["zona"]=zona; j["trafo"]=trafo; j["calc_con_trafo"]=calcConTrafo; j["compensacion"]=compensacion;
    if(!saiAlimentacion.empty()) j["sai_alimentacion"]=saiAlimentacion;
    j["tag_emer"] = tagEmer ? json(*tagEmer) : json(nullptr); j["ke"]=ke;
    j["cargas"]=json::array(); j["baterias"]=json::array(); j["subcuadros"]=json::array();
    for(const Carga& c:cargas) j["cargas"].push_back(c.toJson());
    for(const Bateria& b:baterias) j["baterias"].push_back(b.toJson());
    for(const Cuadro& s:subcuadros) j["subcuadros"].push_back(s.toJson());
    guardarPos(j,diagX,diagY,diagW,diagH);
    return j;
}
Cuadro Cuadro::fromJson(const json& j){Cuadro c;c.rev=j.value("rev","");c.tag=j.value("tag","");c.nombre=j.value("nombre","");c.tipo=j.value("tipo","GENERAL BT");c.configFases=j.value("config_fases","3F+N");c.zona=j.value("zona","");c.trafo=j.value("trafo","");c.calcConTrafo=j.value("calc_con_trafo",false);c.saiAlimentacion=j.value("sai_alimentacion","");if(j.contains("tag_emer")&&!j["tag_emer"].is_null())c.tagEmer=j["tag_emer"].get<std::string>();c.ke=j.value("ke",1.0);for(const json& x:j.value("cargas",json::array()))c.cargas.push_back(Carga::fromJson(x));for(const json& x:j.value("baterias",json::array()))c.baterias.push_back(Bateria::fromJson(x));for(const json& x:j.value("subcuadros",json::array()))c.subcuadros.push_back(Cuadro::fromJson(x));c.compensacion=j.value("compensacion",!c.baterias.empty());leerPos(j,c.diagX,c.diagY,c.diagW,c.diagH);return c;}

json EquipoEmergencia::toJson() const {
    json j=json::object(); j["rev"]=rev; j["tag"]=tag; j["nombre"]=nombre; j["tipo"]=tipo;
    j["forzar_trifasico"]=forzarTrifasico; j["horas"]=horas;
    j["v_deposito_incl_l"]=vDepositoInclL; j["consumo_l_h"]=consumoLH; j["eficiencia_pct"]=eficienciaPct;
    if(!cuadroAlimentacion.empty()) j["cuadro_alimentacion"]=cuadroAlimentacion;
    guardarPos(j,diagX,diagY,diagW,diagH);
    return j;
}
EquipoEmergencia EquipoEmergencia::fromJson(const json& j){EquipoEmergencia e;e.rev=j.value("rev","");e.tag=j.value("tag","");e.nombre=j.value("nombre","");e.tipo=j.value("tipo","UPS");e.forzarTrifasico=j.value("forzar_trifasico",false);e.horas=j.value("horas",1.0);e.vDepositoInclL=j.value("v_deposito_incl_l",0.0);e.consumoLH=j.value("consumo_l_h",0.0);e.eficienciaPct=j.value("eficiencia_pct",99.0);e.cuadroAlimentacion=j.value("cuadro_alimentacion","");leerPos(j,e.diagX,e.diagY,e.diagW,e.diagH);return e;}

json Proyecto::toJson() const {json dg={{"tags_automaticos",datos.tagsAutomaticos},{"tablas_automaticas",datos.tablasAutomaticas},{"p_min_1f_3f_kw",datos.pMin1F3FKw},{"transformadores",json::array()},{"zonas",json::array()},{"portada",{{"empresa",datos.portada.empresa},{"codigo_documento",datos.portada.codigoDocumento},{"titulo",datos.portada.titulo},{"escrito",datos.portada.escrito},{"comprobado",datos.portada.comprobado},{"verificado",datos.portada.verificado}}}};for(const Transformador& t:datos.transformadores){json jt={{"tag",t.tag},{"s_nom_kva",t.sNomKva},{"s_adm_kva",t.sAdmKva},{"u_sec_v",t.uSecV},{"n_paralelo",t.nParalelo}};guardarPos(jt,t.diagX,t.diagY,t.diagW,t.diagH);dg["transformadores"].push_back(jt);}dg["tipos_carga"]=json::array();for(const TipoCargaDef& t:datos.tiposCarga)dg["tipos_carga"].push_back({{"nombre",t.nombre},{"prefijo",t.prefijo},{"descripcion",t.descripcion},{"cos_fijo",t.cosFijo?json(*t.cosFijo):json(nullptr)},{"rendimiento",t.rendimiento}});dg["prefijo_bateria"]=datos.prefijoBateria;for(const Zona& z:datos.zonas)dg["zonas"].push_back({{"tag",z.tag},{"descripcion",z.descripcion}});dg["fotovoltaica"]=json::array();for(const InstalacionFV& f:datos.fotovoltaica){json jf={{"rev",f.rev},{"tag",f.tag},{"nombre",f.nombre},{"p_pico_kwp",f.pPicoKwp},{"p_inversor_kw",f.pInversorKw},{"n_inversores",f.nInversores},{"config_fases",f.configFases},{"cuadro",f.cuadro}};guardarPos(jf,f.diagX,f.diagY,f.diagW,f.diagH);dg["fotovoltaica"].push_back(jf);}json j={{"formato","lista-cargas"},{"version",1},{"datos_generales",dg},{"cuadros",json::array()},{"equipos_emergencia",json::array()}};for(const Cuadro& c:cuadros)j["cuadros"].push_back(c.toJson());for(const EquipoEmergencia& e:equipos)j["equipos_emergencia"].push_back(e.toJson());
    json ea=json::object();
    for(const auto& [k,e]:estilosAristas){
        if(e.vacio()) continue;
        json x=json::object();
        if(e.codo) x["codo"]=*e.codo;
        if(e.ladoOrigen>=0) x["lado_origen"]=e.ladoOrigen;
        if(e.ladoDestino>=0) x["lado_destino"]=e.ladoDestino;
        if(e.fracOrigen!=0.5) x["frac_origen"]=e.fracOrigen;
        if(e.fracDestino!=0.5) x["frac_destino"]=e.fracDestino;
        ea[k]=x;
    }
    if(!ea.empty()) j["diagrama_flechas"]=ea;
    return j;}
Proyecto Proyecto::fromJson(const json& j){if(j.value("formato","")!="lista-cargas")throw std::runtime_error("El fichero no es una lista de cargas (formato desconocido).");if(j.value("version",0)>1)throw std::runtime_error("El fichero pertenece a una versión más reciente.");Proyecto p;const json& dg=j.value("datos_generales",json::object());p.datos.tagsAutomaticos=dg.value("tags_automaticos",true);p.datos.tablasAutomaticas=dg.value("tablas_automaticas",true);p.datos.pMin1F3FKw=dg.value("p_min_1f_3f_kw",5.0);for(const json& t:dg.value("transformadores",json::array())){Transformador x;x.tag=t.value("tag","");x.sNomKva=t.value("s_nom_kva",0.0);x.sAdmKva=t.value("s_adm_kva",0.0);x.uSecV=t.value("u_sec_v",400.0);x.nParalelo=std::max(1,t.value("n_paralelo",1));leerPos(t,x.diagX,x.diagY,x.diagW,x.diagH);p.datos.transformadores.push_back(x);}if(dg.contains("tipos_carga")&&dg["tipos_carga"].is_array()){
        p.datos.tiposCarga.clear();
        for(const json& t:dg["tipos_carga"]){
            TipoCargaDef x;x.nombre=t.value("nombre","");x.prefijo=t.value("prefijo","");x.descripcion=t.value("descripcion","");
            if(t.contains("cos_fijo")&&t["cos_fijo"].is_number())x.cosFijo=t["cos_fijo"].get<double>();
            x.rendimiento=t.value("rendimiento",0);
            if(!x.nombre.empty())p.datos.tiposCarga.push_back(x);
        }
    }
    p.datos.prefijoBateria=dg.value("prefijo_bateria","CAP");
    // Ficheros anteriores: prefijos personalizados por tipo («prefijos_tag»)
    if(dg.contains("prefijos_tag")&&dg["prefijos_tag"].is_object())
        for(const auto& [k,v]:dg["prefijos_tag"].items()){
            if(!v.is_string()||v.get<std::string>().empty())continue;
            if(k==TIPO_BATERIA){p.datos.prefijoBateria=v.get<std::string>();continue;}
            for(TipoCargaDef& t:p.datos.tiposCarga)if(t.nombre==k)t.prefijo=v.get<std::string>();
        }for(const json& z:dg.value("zonas",json::array()))p.datos.zonas.push_back({z.value("tag",""),z.value("descripcion","")});for(const json& f:dg.value("fotovoltaica",json::array())){InstalacionFV x;x.rev=f.value("rev","");x.tag=f.value("tag","");x.nombre=f.value("nombre","");x.pPicoKwp=f.value("p_pico_kwp",0.0);x.pInversorKw=f.value("p_inversor_kw",0.0);x.nInversores=f.value("n_inversores",1);x.configFases=f.value("config_fases","3F+N");x.cuadro=f.value("cuadro","");leerPos(f,x.diagX,x.diagY,x.diagW,x.diagH);p.datos.fotovoltaica.push_back(x);}const json& po=dg.value("portada",json::object());p.datos.portada={po.value("empresa",""),po.value("codigo_documento",""),po.value("titulo","LISTA DE CARGA"),po.value("escrito",""),po.value("comprobado",""),po.value("verificado","")};for(const json& c:j.value("cuadros",json::array()))p.cuadros.push_back(Cuadro::fromJson(c));for(const json& e:j.value("equipos_emergencia",json::array()))p.equipos.push_back(EquipoEmergencia::fromJson(e));
    if(j.contains("diagrama_flechas")&&j["diagrama_flechas"].is_object())
        for(const auto& [k,x]:j["diagrama_flechas"].items()){
            if(!x.is_object()) continue;
            EstiloArista e;
            if(x.contains("codo")&&x["codo"].is_number()) e.codo=x["codo"].get<double>();
            e.ladoOrigen=x.value("lado_origen",-1); e.ladoDestino=x.value("lado_destino",-1);
            e.fracOrigen=x.value("frac_origen",0.5); e.fracDestino=x.value("frac_destino",0.5);
            if(!e.vacio()) p.estilosAristas[k]=e;
        }
    p.recalcular();return p;}

Proyecto abrirProyecto(const std::string& ruta){std::ifstream f(ruta);if(!f)throw std::runtime_error("No se pudo abrir el fichero.");json j;f>>j;return Proyecto::fromJson(j);}
void guardarProyecto(const Proyecto& proyecto,const std::string& ruta){std::ofstream f(ruta,std::ios::trunc);if(!f)throw std::runtime_error("No se pudo crear el fichero.");Proyecto copy=proyecto;copy.recalcular();f<<copy.toJson().dump(2)<<"\n";}
