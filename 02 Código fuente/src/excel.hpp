#pragma once

#include <string>

#include "model.hpp"

// Exporta la lista de cargas del proyecto a un libro de Excel (.xlsx).
// El libro contiene las hojas «Lista de cargas», «Cuadros», «Emergencia» y «Avisos».
// No necesita bibliotecas externas: el .xlsx (OOXML dentro de un ZIP) se genera a mano.
void exportarExcel(const Proyecto& proyecto, const std::string& ruta);
