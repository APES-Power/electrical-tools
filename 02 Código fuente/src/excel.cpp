#include "excel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

// ─────────────────────────────────────────────────────────────────────────────
//  ZIP mínimo (entradas sin comprimir, método «stored»)
// ─────────────────────────────────────────────────────────────────────────────

uint32_t crc32(const std::string& data) {
    static const std::array<uint32_t, 256> tabla = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b: data) c = tabla[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void le16(std::string& out, uint16_t v) { out.push_back(char(v & 0xFF)); out.push_back(char(v >> 8)); }
void le32(std::string& out, uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(char((v >> (8 * i)) & 0xFF)); }

struct EntradaZip { std::string nombre, datos; };

std::string crearZip(const std::vector<EntradaZip>& entradas) {
    std::time_t ahora = std::time(nullptr);
    std::tm tm{};
    localtime_r(&ahora, &tm);
    uint16_t hora = uint16_t((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    uint16_t fecha = uint16_t(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);

    std::string zip, central;
    for (const EntradaZip& e: entradas) {
        uint32_t crc = crc32(e.datos);
        uint32_t offset = uint32_t(zip.size());
        uint32_t tam = uint32_t(e.datos.size());

        le32(zip, 0x04034b50); le16(zip, 20); le16(zip, 0x0800); le16(zip, 0);
        le16(zip, hora); le16(zip, fecha); le32(zip, crc); le32(zip, tam); le32(zip, tam);
        le16(zip, uint16_t(e.nombre.size())); le16(zip, 0);
        zip += e.nombre;
        zip += e.datos;

        le32(central, 0x02014b50); le16(central, 20); le16(central, 20); le16(central, 0x0800); le16(central, 0);
        le16(central, hora); le16(central, fecha); le32(central, crc); le32(central, tam); le32(central, tam);
        le16(central, uint16_t(e.nombre.size())); le16(central, 0); le16(central, 0); le16(central, 0); le16(central, 0);
        le32(central, 0); le32(central, offset);
        central += e.nombre;
    }
    uint32_t inicioCentral = uint32_t(zip.size());
    zip += central;
    le32(zip, 0x06054b50); le16(zip, 0); le16(zip, 0);
    le16(zip, uint16_t(entradas.size())); le16(zip, uint16_t(entradas.size()));
    le32(zip, uint32_t(central.size())); le32(zip, inicioCentral); le16(zip, 0);
    return zip;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Modelo de hoja de cálculo
// ─────────────────────────────────────────────────────────────────────────────

// Índices de estilo (cellXfs en styles.xml)
enum Estilo : int {
    E_NORMAL = 0, E_CABECERA, E_TEXTO, E_NUM2, E_NUM3, E_TITULO, E_ETIQUETA, E_NUM1,
    E_CUADRO_TEXTO, E_CUADRO_NUM2, E_CUADRO_NUM3, E_ENTERO
};

struct Celda {
    enum Tipo { Vacia, Texto, Numero } tipo = Vacia;
    std::string texto;
    double numero = 0;
    int estilo = E_NORMAL;
};

Celda txt(const std::string& s, int estilo = E_TEXTO) { Celda c; c.tipo = Celda::Texto; c.texto = s; c.estilo = estilo; return c; }
Celda num(double v, int estilo = E_NUM2) {
    Celda c; c.estilo = estilo;
    if (std::isfinite(v)) { c.tipo = Celda::Numero; c.numero = v; }
    return c;
}
Celda vacia(int estilo = E_TEXTO) { Celda c; c.estilo = estilo; return c; }
Celda numOpt(const std::optional<double>& v, int estilo = E_NUM2) { return v ? num(*v, estilo) : vacia(estilo); }

struct Hoja {
    std::string nombre;
    std::vector<std::vector<Celda>> filas;
    std::vector<double> anchos;
    int filaCabecera = -1;   // índice (base 0) de la fila de cabecera de la tabla; -1 = sin tabla
};

std::string escaparXml(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char ch: s) {
        switch (ch) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            default:
                // Los caracteres de control (salvo tab y salto de línea) no son XML válido
                if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') break;
                r.push_back(char(ch));
        }
    }
    return r;
}

std::string columna(size_t i) {
    std::string r;
    size_t n = i + 1;
    while (n > 0) { n--; r.insert(r.begin(), char('A' + n % 26)); n /= 26; }
    return r;
}

std::string numeroXml(double v) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s.precision(15);
    s << v;
    return s.str();
}

size_t columnasDe(const Hoja& h) {
    if (!h.anchos.empty()) return h.anchos.size();   // ancho de la tabla
    size_t n = 0;
    for (const auto& f: h.filas) n = std::max(n, f.size());
    return std::max(n, h.anchos.size());
}

std::string xmlHoja(const Hoja& h) {
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
         "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
    x << "<sheetViews><sheetView workbookViewId=\"0\"";
    if (h.filaCabecera >= 0) {
        int y = h.filaCabecera + 1;
        x << "><pane ySplit=\"" << y << "\" topLeftCell=\"A" << (y + 1)
          << "\" activePane=\"bottomLeft\" state=\"frozen\"/>"
             "<selection pane=\"bottomLeft\" activeCell=\"A" << (y + 1) << "\" sqref=\"A" << (y + 1) << "\"/></sheetView>";
    } else {
        x << "/>";
    }
    x << "</sheetViews><sheetFormatPr defaultRowHeight=\"15\"/>";
    if (!h.anchos.empty()) {
        x << "<cols>";
        for (size_t i = 0; i < h.anchos.size(); i++)
            x << "<col min=\"" << (i + 1) << "\" max=\"" << (i + 1) << "\" width=\"" << numeroXml(h.anchos[i]) << "\" customWidth=\"1\"/>";
        x << "</cols>";
    }
    x << "<sheetData>";
    for (size_t r = 0; r < h.filas.size(); r++) {
        const auto& fila = h.filas[r];
        x << "<row r=\"" << (r + 1) << "\"";
        if (int(r) == h.filaCabecera) x << " ht=\"32\" customHeight=\"1\"";
        x << ">";
        for (size_t c = 0; c < fila.size(); c++) {
            const Celda& celda = fila[c];
            std::string ref = columna(c) + std::to_string(r + 1);
            if (celda.tipo == Celda::Texto) {
                x << "<c r=\"" << ref << "\" s=\"" << celda.estilo << "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">"
                  << escaparXml(celda.texto) << "</t></is></c>";
            } else if (celda.tipo == Celda::Numero) {
                x << "<c r=\"" << ref << "\" s=\"" << celda.estilo << "\"><v>" << numeroXml(celda.numero) << "</v></c>";
            } else if (celda.estilo != E_NORMAL) {
                x << "<c r=\"" << ref << "\" s=\"" << celda.estilo << "\"/>";
            }
        }
        x << "</row>";
    }
    x << "</sheetData>";
    if (h.filaCabecera >= 0 && h.filas.size() > size_t(h.filaCabecera)) {
        x << "<autoFilter ref=\"A" << (h.filaCabecera + 1) << ":" << columna(columnasDe(h) - 1) << h.filas.size() << "\"/>";
    }
    x << "<pageMargins left=\"0.5\" right=\"0.5\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
         "<pageSetup paperSize=\"9\" orientation=\"landscape\" fitToHeight=\"0\"/>"
         "</worksheet>";
    return x.str();
}

const char* ESTILOS_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<numFmts count=\"3\">"
    "<numFmt numFmtId=\"164\" formatCode=\"#,##0.00\"/>"
    "<numFmt numFmtId=\"165\" formatCode=\"0.000\"/>"
    "<numFmt numFmtId=\"166\" formatCode=\"0.0\"/>"
    "</numFmts>"
    "<fonts count=\"4\">"
    "<font><sz val=\"10\"/><name val=\"Arial\"/><family val=\"2\"/></font>"
    "<font><b/><sz val=\"10\"/><name val=\"Arial\"/><family val=\"2\"/></font>"
    "<font><b/><sz val=\"10\"/><color rgb=\"FFFFFFFF\"/><name val=\"Arial\"/><family val=\"2\"/></font>"
    "<font><b/><sz val=\"14\"/><name val=\"Arial\"/><family val=\"2\"/></font>"
    "</fonts>"
    "<fills count=\"4\">"
    "<fill><patternFill patternType=\"none\"/></fill>"
    "<fill><patternFill patternType=\"gray125\"/></fill>"
    "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF1F4E78\"/><bgColor indexed=\"64\"/></patternFill></fill>"
    "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFDDEBF7\"/><bgColor indexed=\"64\"/></patternFill></fill>"
    "</fills>"
    "<borders count=\"2\">"
    "<border><left/><right/><top/><bottom/><diagonal/></border>"
    "<border><left style=\"thin\"><color rgb=\"FFBFBFBF\"/></left><right style=\"thin\"><color rgb=\"FFBFBFBF\"/></right>"
    "<top style=\"thin\"><color rgb=\"FFBFBFBF\"/></top><bottom style=\"thin\"><color rgb=\"FFBFBFBF\"/></bottom><diagonal/></border>"
    "</borders>"
    "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
    "<cellXfs count=\"12\">"
    "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"                                                    // E_NORMAL
    "<xf numFmtId=\"0\" fontId=\"2\" fillId=\"2\" borderId=\"1\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\" applyAlignment=\"1\">"
    "<alignment horizontal=\"center\" vertical=\"center\" wrapText=\"1\"/></xf>"                                                   // E_CABECERA
    "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyBorder=\"1\"/>"                                   // E_TEXTO
    "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyBorder=\"1\"/>"         // E_NUM2
    "<xf numFmtId=\"165\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyBorder=\"1\"/>"         // E_NUM3
    "<xf numFmtId=\"0\" fontId=\"3\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/>"                                     // E_TITULO
    "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/>"                                     // E_ETIQUETA
    "<xf numFmtId=\"166\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyBorder=\"1\"/>"         // E_NUM1
    "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>"   // E_CUADRO_TEXTO
    "<xf numFmtId=\"164\" fontId=\"1\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>" // E_CUADRO_NUM2
    "<xf numFmtId=\"165\" fontId=\"1\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>" // E_CUADRO_NUM3
    "<xf numFmtId=\"1\" fontId=\"0\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyBorder=\"1\"/>"           // E_ENTERO
    "</cellXfs>"
    "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
    "</styleSheet>";

std::string crearXlsx(const std::vector<Hoja>& hojas, const Portada& portada) {
    std::vector<EntradaZip> e;

    std::ostringstream tipos;
    tipos << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
             "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
             "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
             "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
             "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
             "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
             "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>";
    for (size_t i = 0; i < hojas.size(); i++)
        tipos << "<Override PartName=\"/xl/worksheets/sheet" << (i + 1)
              << ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    tipos << "</Types>";
    e.push_back({"[Content_Types].xml", tipos.str()});

    e.push_back({"_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
        "</Relationships>"});

    std::ostringstream core;
    core << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
         << "<dc:title>Lista de cargas</dc:title>"
         << "</cp:coreProperties>";
    (void)portada;
    e.push_back({"docProps/core.xml", core.str()});

    std::ostringstream wb, wbRels;
    wb << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
          "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>";
    wbRels << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
              "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (size_t i = 0; i < hojas.size(); i++) {
        wb << "<sheet name=\"" << escaparXml(hojas[i].nombre) << "\" sheetId=\"" << (i + 1) << "\" r:id=\"rId" << (i + 1) << "\"/>";
        wbRels << "<Relationship Id=\"rId" << (i + 1)
               << "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet"
               << (i + 1) << ".xml\"/>";
    }
    wb << "</sheets>";
    std::ostringstream nombres;
    for (size_t i = 0; i < hojas.size(); i++) {
        const Hoja& h = hojas[i];
        if (h.filaCabecera < 0 || h.filas.size() <= size_t(h.filaCabecera)) continue;
        nombres << "<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"" << i << "\" hidden=\"1\">'"
           << escaparXml(h.nombre) << "'!$A$" << (h.filaCabecera + 1) << ":$" << columna(columnasDe(h) - 1) << "$" << h.filas.size()
           << "</definedName>";
    }
    if (!nombres.str().empty()) wb << "<definedNames>" << nombres.str() << "</definedNames>";
    wb << "</workbook>";
    wbRels << "<Relationship Id=\"rId" << (hojas.size() + 1)
           << "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
              "</Relationships>";
    e.push_back({"xl/workbook.xml", wb.str()});
    e.push_back({"xl/_rels/workbook.xml.rels", wbRels.str()});
    e.push_back({"xl/styles.xml", ESTILOS_XML});
    for (size_t i = 0; i < hojas.size(); i++)
        e.push_back({"xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", xmlHoja(hojas[i])});
    return crearZip(e);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Contenido de las hojas
// ─────────────────────────────────────────────────────────────────────────────

std::string unir(const std::vector<std::string>& v, const char* sep = "; ") {
    std::string r;
    for (size_t i = 0; i < v.size(); i++) { if (i) r += sep; r += v[i]; }
    return r;
}

void encabezado(Hoja& h, const Portada& p, const std::string& subtitulo) {
    (void)p;   // los datos de portada (empresa, código...) se gestionan fuera de la aplicación
    h.filas.push_back({txt("LISTA DE CARGAS", E_TITULO)});
    h.filas.push_back({txt(subtitulo, E_ETIQUETA)});
    h.filas.push_back({});
}

void cabecera(Hoja& h, const std::vector<std::pair<std::string, double>>& columnas) {
    std::vector<Celda> fila;
    for (const auto& [titulo, ancho]: columnas) { fila.push_back(txt(titulo, E_CABECERA)); h.anchos.push_back(ancho); }
    h.filaCabecera = int(h.filas.size());
    h.filas.push_back(fila);
}

using Visita = std::function<void(const Cuadro&, const std::string& padre)>;
void recorrer(const std::vector<Cuadro>& cuadros, const std::string& padre, const Visita& fn) {
    for (const Cuadro& c: cuadros) { fn(c, padre); recorrer(c.subcuadros, c.tag, fn); }
}

Hoja hojaCargas(const Proyecto& p) {
    Hoja h; h.nombre = "Lista de cargas";
    encabezado(h, p.datos.portada, "Lista de cargas");
    cabecera(h, {
        {"Cuadro", 12}, {"Cuadro aguas arriba", 12}, {"Zona", 8}, {"Trafo", 8}, {"Rev.", 6},
        {"TAG", 14}, {"Nombre", 34}, {"Tipo", 14}, {"Longitud\n[m]", 9}, {"P nominal\n[kW]", 11}, {"Fases", 8}, {"U\n[V]", 7},
        {"cos φ", 8}, {"η\n[%]", 7}, {"P absorbida\n[kW]", 12}, {"Q absorbida\n[kVAr]", 12},
        {"Ku", 6}, {"Ks", 6}, {"Ke", 6},
        {"P demandada\n[kW]", 12}, {"Q demandada\n[kVAr]", 12}, {"S demandada\n[kVA]", 12}, {"S emergencia\n[kVA]", 12},
        {"Equipo emerg.", 12}, {"Avisos", 40},
    });

    recorrer(p.cuadros, "", [&](const Cuadro& cu, const std::string& padre) {
        // Fila de cabecera del cuadro con sus totales
        h.filas.push_back({
            txt(cu.tag, E_CUADRO_TEXTO), txt(padre, E_CUADRO_TEXTO), txt(cu.zonaEfectiva, E_CUADRO_TEXTO),
            txt(cu.trafoEfectivo, E_CUADRO_TEXTO), vacia(E_CUADRO_TEXTO), txt(cu.tag, E_CUADRO_TEXTO),
            txt(cu.nombre, E_CUADRO_TEXTO), txt(cu.tipo, E_CUADRO_TEXTO), vacia(E_CUADRO_TEXTO), num(cu.pInstKw, E_CUADRO_NUM2),
            txt(cu.configFases, E_CUADRO_TEXTO), num(cu.tensionV, E_CUADRO_NUM2), num(cu.cosPhi, E_CUADRO_NUM3),
            vacia(E_CUADRO_TEXTO), num(cu.pCargasKw, E_CUADRO_NUM2), num(cu.qCargasKvar, E_CUADRO_NUM2),
            vacia(E_CUADRO_TEXTO), num(cu.ksGlobal, E_CUADRO_NUM2), num(cu.ke, E_CUADRO_NUM2),
            num(cu.pKw, E_CUADRO_NUM2), num(cu.qKvar, E_CUADRO_NUM2), num(cu.sKva, E_CUADRO_NUM2), num(cu.sEmerKva, E_CUADRO_NUM2),
            txt(cu.tagEmer.value_or(""), E_CUADRO_TEXTO), txt(unir(cu.avisos), E_CUADRO_TEXTO),
        });
        for (const Carga& c: cu.cargas) {
            bool conEta = p.datos.usaRendimiento(c.tipo);
            h.filas.push_back({
                txt(cu.tag), txt(padre), txt(c.zonaEfectiva), txt(cu.trafoEfectivo), txt(c.rev),
                txt(c.tag), txt(c.nombre), txt(p.datos.descripcionTipo(c.tipo)), numOpt(c.longitudM, E_NUM1), num(c.pNomKw), txt(c.fases), num(c.tensionV, E_ENTERO),
                num(c.cosEfectivo, E_NUM3), conEta ? num(c.etaPct, E_NUM1) : vacia(), num(c.nKw), num(c.qAbsKvar),
                num(c.ku), num(c.ks), num(c.ke),
                num(c.pKw), num(c.qKvar), num(c.sKva), num(c.sEmerKva),
                txt(c.tagEmer.value_or("")), txt(unir(c.avisos)),
            });
        }
        for (const Bateria& b: cu.baterias) {
            h.filas.push_back({
                txt(cu.tag), txt(padre), txt(cu.zonaEfectiva), txt(cu.trafoEfectivo), vacia(),
                txt(b.tag), txt(b.nombre), txt(p.datos.descripcionTipo(TIPO_BATERIA)), vacia(), vacia(), txt(b.configFases), num(b.tensionV(), E_ENTERO),
                vacia(), vacia(), vacia(), vacia(), vacia(), vacia(), vacia(),
                num(0), num(b.qKvar()), num(std::abs(b.qKvar())), vacia(), vacia(), txt(unir(b.avisos)),
            });
        }
    });
    return h;
}

Hoja hojaCuadros(const Proyecto& p) {
    Hoja h; h.nombre = "Cuadros";
    encabezado(h, p.datos.portada, "Resumen por cuadro");
    cabecera(h, {
        {"TAG", 12}, {"Nombre", 34}, {"Tipo", 14}, {"Cuadro aguas arriba", 12}, {"Zona", 8}, {"Trafo", 8},
        {"Fases", 8}, {"U\n[V]", 7}, {"Nº cargas", 8}, {"P instalada\n[kW]", 12}, {"P absorbida cargas\n[kW]", 13},
        {"Q absorbida cargas\n[kVAr]", 13}, {"Ks global", 9}, {"P demandada\n[kW]", 12}, {"Q demandada\n[kVAr]", 12},
        {"S demandada\n[kVA]", 12}, {"cos φ", 8}, {"Ke", 6}, {"S emergencia\n[kVA]", 12}, {"S dimensionado\n[kVA]", 13},
        {"Equipo emerg.", 12}, {"Avisos", 40},
    });
    recorrer(p.cuadros, "", [&](const Cuadro& cu, const std::string& padre) {
        h.filas.push_back({
            txt(cu.tag), txt(cu.nombre), txt(cu.tipo), txt(padre), txt(cu.zonaEfectiva), txt(cu.trafoEfectivo),
            txt(cu.configFases), num(cu.tensionV, E_ENTERO), num(double(cu.cargas.size()), E_ENTERO),
            num(cu.pInstKw), num(cu.pCargasKw), num(cu.qCargasKvar), num(cu.ksGlobal, E_NUM3),
            num(cu.pKw), num(cu.qKvar), num(cu.sKva), num(cu.cosPhi, E_NUM3), num(cu.ke),
            num(cu.sEmerKva), num(cu.sDimensionKva), txt(cu.tagEmer.value_or("")), txt(unir(cu.avisos)),
        });
    });
    return h;
}

Hoja hojaEmergencia(const Proyecto& p) {
    Hoja h; h.nombre = "Emergencia";
    encabezado(h, p.datos.portada, "Equipos de emergencia");
    cabecera(h, {
        {"TAG", 10}, {"Nombre", 30}, {"Tipo", 12}, {"Nº consumidores", 12}, {"Fases", 8}, {"U\n[V]", 7},
        {"S emergencia\n[kVA]", 12}, {"S mínima\n(×1,3) [kVA]", 12}, {"S diseño\n[kVA]", 12}, {"Autonomía\n[h]", 10},
        {"Depósito adicional\n[L]", 13}, {"Capacidad batería\n[kWh]", 13}, {"Avisos", 40},
    });
    for (const EquipoEmergencia& e: p.equipos) {
        h.filas.push_back({
            txt(e.tag), txt(e.nombre), txt(e.tipo == "UPS" ? "SAI" : e.tipo == "G. ELEC." ? "GRUPO ELECTRÓGENO" : e.tipo), num(e.nConsumidores, E_ENTERO), txt(e.fases), num(e.tensionV, E_ENTERO),
            num(e.sEmerKva), num(e.sEspMinKva), numOpt(e.sDisenoKva), num(e.horas, E_NUM1),
            numOpt(e.vDepositoAdL, E_NUM1), numOpt(e.capBateriaKwh), txt(unir(e.avisos)),
        });
    }
    return h;
}

Hoja hojaAvisos(const Proyecto& p) {
    Hoja h; h.nombre = "Avisos";
    encabezado(h, p.datos.portada, "Avisos del cálculo");
    cabecera(h, {{"Aviso", 100}});
    if (p.avisos.empty()) h.filas.push_back({txt("Sin avisos")});
    for (const std::string& a: p.avisos) h.filas.push_back({txt(a)});
    return h;
}

}  // namespace

void exportarExcel(const Proyecto& proyecto, const std::string& ruta) {
    Proyecto p = proyecto;
    p.recalcular();

    std::vector<Hoja> hojas = {hojaCargas(p), hojaCuadros(p)};
    if (!p.equipos.empty()) hojas.push_back(hojaEmergencia(p));
    hojas.push_back(hojaAvisos(p));

    std::string datos = crearXlsx(hojas, p.datos.portada);
    std::ofstream f(ruta, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("No se pudo crear el fichero «" + ruta + "».");
    f.write(datos.data(), std::streamsize(datos.size()));
    if (!f) throw std::runtime_error("Error al escribir el fichero «" + ruta + "».");
}
