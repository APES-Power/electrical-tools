#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
lista_cargas.py - Lista de cargas eléctricas (BT) en terminal.

Conversión a Python del Excel «250XX-LST-ELE-XX-2900 Lista de cargas».

Flujo del programa
  1. Condiciones generales (tags automáticos, tablas automáticas, P mín. 1F→3F,
     transformadores, zonas, datos de portada).
  2. Cuadros eléctricos. Cada cuadro admite cargas, baterías de condensadores y
     subcuadros (un subcuadro ES un Cuadro, con exactamente las mismas opciones,
     y puede anidarse a cualquier profundidad).
  3. Equipos de emergencia (grupo electrógeno / SAI), dimensionados a partir de
     las cargas que tienen asociado su tag.
  4. Resúmenes y cálculos.

Los datos se guardan en un fichero *.list (JSON legible, sólo datos de entrada;
todos los cálculos se rehacen al abrir).

Uso:
    python3 lista_cargas.py [fichero.list]
    python3 lista_cargas.py fichero.list --resumen     # imprime cálculos y sale
    python3 lista_cargas.py [fichero.list] --gui       # interfaz gráfica (= lista_cargas_gui.py)

Sólo usa la biblioteca estándar (Python >= 3.9).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile
from bisect import bisect_left
from dataclasses import asdict, dataclass, field
from typing import Callable, Iterator, Optional

FORMATO = "lista-cargas"
VERSION = 1
EXTENSION = ".list"

# ═════════════════════════════════════════════════════════════════════════════
#  TABLAS (hoja «LUTs» del Excel)
# ═════════════════════════════════════════════════════════════════════════════

CONFIG_FASES = ("1F+N", "3F", "3F+N")
TENSION_POR_FASES = {"1F+N": 230.0, "3F": 400.0, "3F+N": 400.0}

# tipo -> (tag, descripción, cos φ fijo | None = se calcula con la tabla de motores)
TIPOS_CARGA = {
    "AL. EMER.":  ("AEM",  "ALUMBRADO DE EMERGENCIA", 0.95),
    "ALUMB.":     ("AL",   "ALUMBRADO", 0.95),
    "CCTV":       ("CCTV", "CÁMARAS DE VIDEOVIGILANCIA", 0.90),
    "CPD":        ("CPD",  "CENTRO DE PROCESAMIENTO DE DATOS", 0.90),
    "FOTOVOLT.":  ("PV",   "GENERADOR FOTOVOLTAICO", 1.00),
    "FUERZA":     ("TF",   "TOMA DE FUERZA", 0.85),
    "GENERAL":    ("GEN",  "GENERAL", 0.85),
    "GENSET":     ("G",    "GENERADOR DE APOYO", 0.80),
    "HVAC":       ("HVAC", "CLIMATIZACIÓN GENERAL (HVAC)", None),
    "I&C":        ("IC",   "INSTRUMENTACIÓN Y CONTROL", 0.90),
    "M+VARIADOR": ("VAR",  "MOTOR CON VARIADOR DE FRECUENCIA", 0.95),
    "MOTOR":      ("M",    "MOTOR", None),
    "PUESTO OF.": ("PT",   "PUESTOS DE TRABAJO OFICINAS", 0.85),
    "RECARGA VE": ("VE",   "PUNTO DE RECARGA VEHÍCULO ELÉCTRICO", 1.00),
    "RESERVA":    ("RES",  "RESERVA", 1.00),
    "SPLIT":      ("SPL",  "SPLIT", 0.80),
    "SS.AA.":     ("SSAA", "SERVICIOS AUXILIARES", 0.85),
    "SUM. COMP.": ("SUM",  "SUMINISTRO COMPLEMENTARIO", 0.80),
}
TIPOS_CON_RENDIMIENTO = ("MOTOR", "HVAC")   # la potencia absorbida es P/η
TIPO_BATERIA = "BAT. COND."
TAG_BATERIA = "CAP"

TIPOS_CUADRO = {
    "GENERAL BT":  ("CGBT", "CUADRO GENERAL DE BAJA TENSIÓN"),
    "GEN. DIST.":  ("CGD",  "CUADRO GENERAL DE DISTRIBUCIÓN"),
    "DIST. SEC.":  ("CDS",  "CUADRO DE DISTRIBUCIÓN SECUNDARIO"),
    "PROT. MED.":  ("CPM",  "CUADRO DE PROTECCIÓN Y MEDIDA"),
    "ALUM.":       ("CA",   "CUADRO DE ALUMBRADO"),
    "FUERZA":      ("CF",   "CUADRO DE FUERZA"),
    "F+AL":        ("CFA",  "CUADRO DE FUERZA Y ALUMBRADO"),
}

TIPOS_EMERGENCIA = {
    "G. ELEC.": ("GE",  "GRUPO ELECTRÓGENO"),
    "UPS":      ("UPS", "SISTEMA DE ALIMENTACIÓN ININTERRUMPIDA"),
}

# Motores: potencia [kW], cos φ y rendimiento η (interpolación lineal).
# OJO: en el Excel las filas están rotuladas al revés («η» contiene los cos φ y
# «cos φ» contiene los rendimientos); aquí se usan con su significado físico.
LUT_MOTOR_P = [0.37, 0.55, 0.75, 1.1, 1.5, 2.2, 3.0, 4.0, 5.5, 7.5, 11.0, 15.0, 18.5, 22.0,
               30.0, 37.0, 45.0, 55.0, 75.0, 90.0, 110.0, 132.0, 160.0, 200.0, 250.0, 315.0,
               355.0, 400.0, 450.0, 500.0, 560.0, 630.0, 710.0]
LUT_MOTOR_COS = [0.75, 0.73, 0.76, 0.78, 0.78, 0.80, 0.82, 0.81, 0.84, 0.85, 0.81, 0.84, 0.84,
                 0.83, 0.83, 0.84, 0.83, 0.86, 0.86, 0.87, 0.87, 0.87, 0.86, 0.86, 0.87, 0.87,
                 0.87, 0.87, 0.87, 0.88, 0.90, 0.87, 0.87]
LUT_MOTOR_ETA = [0.708, 0.75, 0.763, 0.785, 0.805, 0.825, 0.845, 0.86, 0.874, 0.89, 0.903,
                 0.911, 0.923, 0.924, 0.929, 0.936, 0.942, 0.946, 0.95, 0.952, 0.956, 0.958,
                 0.96, 0.962, 0.965, 0.967, 0.967, 0.968, 0.968, 0.968, 0.969, 0.969, 0.969]

# Grupos electrógenos [kVA]. El Excel trae «200» entre 1925 y 2200 (errata); se usa 2000.
LUT_GENERADORES = [4.8, 6.0, 8.2, 10.0, 11.0, 14.0, 20.0, 25.0, 33.0, 34.0, 41.0, 48.0, 62.0,
                   68.0, 78.0, 93.0, 110.0, 135.0, 145.0, 170.0, 200.0, 225.0, 275.0, 300.0,
                   330.0, 355.0, 400.0, 450.0, 500.0, 550.0, 600.0, 630.0, 660.0, 700.0, 730.0,
                   800.0, 880.0, 915.0, 1000.0, 1100.0, 1375.0, 1650.0, 1875.0, 1925.0,
                   2000.0, 2200.0]

# SAI [kVA]
LUT_SAI_1F = [0.7, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0, 15.0, 20.0]
LUT_SAI_3F = [10.0, 15.0, 20.0, 25.0, 30.0, 40.0, 50.0, 60.0, 80.0, 100.0, 120.0, 160.0, 200.0,
              250.0, 300.0, 400.0, 500.0]

# Variadores: potencia de motor [kW] -> potencia del variador [kVA]
LUT_VAR_1F_MOTOR = [0.18, 0.37, 0.55, 0.75, 1.1, 1.5, 2.2, 3.0, 4.0, 5.5]
LUT_VAR_1F_INV = [0.6, 1.0, 1.4, 1.8, 2.4, 3.2, 4.4, 9.1, 12.4, 16.7]
LUT_VAR_3F_MOTOR = [0.18, 0.37, 0.55, 0.75, 1.2, 1.5, 2.2, 3.0, 4.0, 5.5, 7.5, 11.0, 15.0, 18.5,
                    22.0, 30.0, 37.0, 45.0, 55.0, 75.0, 90.0, 110.0, 132.0, 160.0, 200.0, 250.0,
                    280.0, 315.0, 355.0, 400.0, 500.0]
LUT_VAR_3F_INV = [0.7, 1.5, 1.8, 2.4, 3.2, 4.2, 5.9, 7.1, 9.5, 15.0, 18.0, 25.0, 32.0, 29.9,
                  33.3, 45.7, 55.3, 68.5, 79.0, 109.9, 109.3, 133.0, 157.3, 190.2, 235.0,
                  292.2, 325.1, 365.3, 419.3, 466.6, 576.6]

# Constantes de dimensionamiento de emergencia (hoja «DIMENSIONAMIENTO EMERGENCIA»)
FACTOR_SESP = 1.3          # S_ESP mín = S_EMER · 1,3
MARGEN_DEPOSITO = 1.05     # V depósito adicional = (consumo·horas − V incluido) · 1,05

EPS = 1e-9


def interpolar(x: float, xs: list, ys: list) -> float:
    """Interpolación lineal con saturación en los extremos de la tabla."""
    if x <= xs[0]:
        return ys[0]
    if x >= xs[-1]:
        return ys[-1]
    i = bisect_left(xs, x)
    if abs(xs[i] - x) < EPS:
        return ys[i]
    x0, x1, y0, y1 = xs[i - 1], xs[i], ys[i - 1], ys[i]
    return y0 + (y1 - y0) * (x - x0) / (x1 - x0)


def siguiente_valor(x: float, valores: list) -> Optional[float]:
    """Menor valor de la lista >= x (None si x excede la tabla)."""
    for v in valores:
        if v >= x - EPS:
            return v
    return None


def potencia_variador(p_motor_kw: float, monofasico: bool) -> Optional[float]:
    motores, inv = ((LUT_VAR_1F_MOTOR, LUT_VAR_1F_INV) if monofasico
                    else (LUT_VAR_3F_MOTOR, LUT_VAR_3F_INV))
    for m, s in zip(motores, inv):
        if m >= p_motor_kw - EPS:
            return s
    return None


def fases_automaticas(p_kw: float, forzar_trifasico: bool, p_min_kw: float) -> str:
    return "3F+N" if (forzar_trifasico or p_kw >= p_min_kw - EPS) else "1F+N"


# ═════════════════════════════════════════════════════════════════════════════
#  MODELO
# ═════════════════════════════════════════════════════════════════════════════

@dataclass
class Portada:
    empresa: str = ""
    codigo_documento: str = ""
    titulo: str = "LISTA DE CARGA"
    escrito: str = ""
    comprobado: str = ""
    verificado: str = ""


@dataclass
class Transformador:
    tag: str
    s_nom_kva: float
    s_adm_kva: float
    u_sec_v: float = 400.0


@dataclass
class Zona:
    tag: str
    descripcion: str = ""


@dataclass
class DatosGenerales:
    tags_automaticos: bool = True
    tablas_automaticas: bool = True     # cos φ y η desde tablas si no se introducen a mano
    p_min_1f_3f_kw: float = 5.0
    transformadores: list = field(default_factory=list)
    zonas: list = field(default_factory=list)
    portada: Portada = field(default_factory=Portada)

    def trafo(self, tag: str) -> Optional[Transformador]:
        return next((t for t in self.transformadores if t.tag == tag), None)

    def to_dict(self) -> dict:
        return {
            "tags_automaticos": self.tags_automaticos,
            "tablas_automaticas": self.tablas_automaticas,
            "p_min_1f_3f_kw": self.p_min_1f_3f_kw,
            "transformadores": [asdict(t) for t in self.transformadores],
            "zonas": [asdict(z) for z in self.zonas],
            "portada": asdict(self.portada),
        }

    @classmethod
    def from_dict(cls, d: dict) -> "DatosGenerales":
        return cls(
            tags_automaticos=bool(d.get("tags_automaticos", True)),
            tablas_automaticas=bool(d.get("tablas_automaticas", True)),
            p_min_1f_3f_kw=float(d.get("p_min_1f_3f_kw", 5.0)),
            transformadores=[Transformador(**t) for t in d.get("transformadores", [])],
            zonas=[Zona(**z) for z in d.get("zonas", [])],
            portada=Portada(**d.get("portada", {})),
        )


class Elemento:
    """Base común: revisión, tag y nombre."""

    def __init__(self, tag: str = "", nombre: str = "", rev: str = ""):
        self.tag = tag
        self.nombre = nombre
        self.rev = rev


class Carga(Elemento):
    """Una carga individual (motor, alumbrado, split…)."""

    def __init__(self, tipo: str, p_nom_kw: float, *, tag: str = "", nombre: str = "",
                 rev: str = "", config_fases: Optional[str] = None,
                 cos_phi: Optional[float] = None, rendimiento_pct: Optional[float] = None,
                 ku: float = 1.0, ks: float = 1.0, ke: float = 1.0,
                 tag_emer: Optional[str] = None):
        super().__init__(tag, nombre, rev)
        if tipo not in TIPOS_CARGA:
            raise ValueError(f"Tipo de carga desconocido: {tipo!r}")
        self.tipo = tipo
        self.p_nom_kw = p_nom_kw
        self.config_fases = config_fases          # None = automático
        self.cos_phi = cos_phi                    # None = tabla
        self.rendimiento_pct = rendimiento_pct    # None = tabla
        self.ku, self.ks, self.ke = ku, ks, ke
        self.tag_emer = tag_emer
        self._reset()

    def _reset(self):
        self.fases = ""
        self.tension_v = 0.0
        self.cos_efectivo = 1.0
        self.eta_pct: Optional[float] = None
        self.n_kw = 0.0          # potencia absorbida (sin Ku/Ks)
        self.q_abs_kvar = 0.0
        self.p_kw = self.q_kvar = self.s_kva = self.s_emer_kva = 0.0
        self.avisos: list = []

    def calcular(self, dg: DatosGenerales, fases_equipo: Optional[str] = None) -> None:
        self._reset()
        p = self.p_nom_kw
        # Fases: valor manual > configuración del equipo de emergencia asociado > 3F+N
        self.fases = self.config_fases or fases_equipo or "3F+N"
        self.tension_v = TENSION_POR_FASES[self.fases]
        if self.fases == "1F+N" and p >= dg.p_min_1f_3f_kw - EPS:
            self.avisos.append(f"carga monofásica con P ≥ {dg.p_min_1f_3f_kw:g} kW (P mín. 1F→3F)")
        fijo = TIPOS_CARGA[self.tipo][2]

        # cos φ
        if self.cos_phi is not None:
            cos = self.cos_phi
        elif dg.tablas_automaticas:
            cos = fijo if fijo is not None else interpolar(p, LUT_MOTOR_P, LUT_MOTOR_COS)
        else:
            cos = 1.0
            self.avisos.append("falta cos φ (tablas automáticas desactivadas); se usa 1,0")
        cos = min(max(cos, 1e-6), 1.0)
        self.cos_efectivo = cos

        # rendimiento (se muestra en motores, HVAC y motor+variador)
        if self.tipo in TIPOS_CON_RENDIMIENTO or self.tipo == "M+VARIADOR":
            if self.rendimiento_pct is not None:
                self.eta_pct = self.rendimiento_pct
            elif dg.tablas_automaticas:
                self.eta_pct = interpolar(p, LUT_MOTOR_P, LUT_MOTOR_ETA) * 100.0
            elif self.tipo in TIPOS_CON_RENDIMIENTO:
                self.eta_pct = 100.0
                self.avisos.append("falta η (tablas automáticas desactivadas); se usa 100 %")

        # potencia absorbida
        if self.tipo in TIPOS_CON_RENDIMIENTO:
            self.n_kw = p / (self.eta_pct / 100.0)
        elif self.tipo == "M+VARIADOR":
            s_inv = potencia_variador(p, self.fases == "1F+N")
            if s_inv is None:
                self.n_kw = p
                self.avisos.append("potencia fuera de la tabla de variadores; se usa P nominal")
            else:
                self.n_kw = s_inv      # el conjunto motor-variador se carga con S del variador
        else:
            self.n_kw = p

        self.q_abs_kvar = self.n_kw * math.tan(math.acos(cos))
        f = self.ku * self.ks
        self.p_kw = self.n_kw * f
        self.q_kvar = self.q_abs_kvar * f
        self.s_kva = math.hypot(self.p_kw, self.q_kvar)
        self.s_emer_kva = self.s_kva * self.ke

    def to_dict(self) -> dict:
        return {"rev": self.rev, "tag": self.tag, "nombre": self.nombre, "tipo": self.tipo,
                "p_nom_kw": self.p_nom_kw, "config_fases": self.config_fases,
                "cos_phi": self.cos_phi, "rendimiento_pct": self.rendimiento_pct,
                "ku": self.ku, "ks": self.ks, "ke": self.ke, "tag_emer": self.tag_emer}

    @classmethod
    def from_dict(cls, d: dict) -> "Carga":
        return cls(d["tipo"], float(d["p_nom_kw"]), tag=d.get("tag", ""),
                   nombre=d.get("nombre", ""), rev=d.get("rev", ""),
                   config_fases=d.get("config_fases"), cos_phi=d.get("cos_phi"),
                   rendimiento_pct=d.get("rendimiento_pct"), ku=float(d.get("ku", 1.0)),
                   ks=float(d.get("ks", 1.0)), ke=float(d.get("ke", 1.0)),
                   tag_emer=d.get("tag_emer"))


class BateriaCondensadores(Elemento):
    """Batería de condensadores: aporta Q capacitiva (no consume P)."""

    def __init__(self, q_kvar: float, *, tag: str = "", nombre: str = "", rev: str = "",
                 config_fases: str = "3F", tension_v: Optional[float] = None):
        super().__init__(tag, nombre, rev)
        self.q_kvar_nominal = q_kvar          # valor positivo
        self.config_fases = config_fases
        self.tension_manual = tension_v

    @property
    def tension_v(self) -> float:
        return self.tension_manual or TENSION_POR_FASES[self.config_fases]

    @property
    def q_kvar(self) -> float:
        return -abs(self.q_kvar_nominal)      # convenio: capacitiva negativa

    def to_dict(self) -> dict:
        return {"rev": self.rev, "tag": self.tag, "nombre": self.nombre,
                "q_kvar": self.q_kvar_nominal, "config_fases": self.config_fases,
                "tension_v": self.tension_manual}

    @classmethod
    def from_dict(cls, d: dict) -> "BateriaCondensadores":
        return cls(float(d["q_kvar"]), tag=d.get("tag", ""), nombre=d.get("nombre", ""),
                   rev=d.get("rev", ""), config_fases=d.get("config_fases", "3F"),
                   tension_v=d.get("tension_v"))


class Cuadro(Elemento):
    """
    Cuadro eléctrico. Un subcuadro es simplemente un Cuadro dentro de `subcuadros`,
    así que cualquier cuadro puede contener cargas, baterías y más subcuadros.
    """

    def __init__(self, tag: str = "", nombre: str = "", *, tipo: str = "GENERAL BT",
                 config_fases: str = "3F+N", zona: str = "", trafo: str = "",
                 calc_con_trafo: bool = False, tag_emer: Optional[str] = None,
                 ke: float = 1.0, rev: str = ""):
        super().__init__(tag, nombre, rev)
        if tipo not in TIPOS_CUADRO:
            raise ValueError(f"Tipo de cuadro desconocido: {tipo!r}")
        self.tipo = tipo
        self.config_fases = config_fases
        self.zona = zona
        self.trafo = trafo
        self.calc_con_trafo = calc_con_trafo
        self.tag_emer = tag_emer
        self.ke = ke
        self.cargas: list[Carga] = []
        self.baterias: list[BateriaCondensadores] = []
        self.subcuadros: list[Cuadro] = []
        self._reset()

    def _reset(self):
        self.zona_efectiva = self.zona
        self.trafo_efectivo = self.trafo
        self.tension_v = 0.0
        self.p_inst_kw = 0.0       # Σ P nominal (cargas + subcuadros)
        self.p_cargas_kw = 0.0     # Σ P absorbida de las cargas propias (sin Ku/Ks)
        self.q_cargas_kvar = 0.0   # Σ Q absorbida de las cargas propias (sin Ku/Ks)
        self.p_kw = self.q_kvar = self.s_kva = 0.0
        self.cos_phi = 1.0
        self.ks_global = 1.0
        self.s_emer_kva = 0.0
        self.s_dimension_kva = 0.0
        self.avisos: list = []

    # ── cálculo ──────────────────────────────────────────────────────────────
    def calcular(self, dg: DatosGenerales, zona_padre: str = "", trafo_padre: str = "",
                 fases_equipos: Optional[dict] = None) -> None:
        self._reset()
        fases_equipos = fases_equipos or {}
        self.zona_efectiva = self.zona or zona_padre
        self.trafo_efectivo = self.trafo or trafo_padre
        trafo = dg.trafo(self.trafo_efectivo) if self.trafo_efectivo else None
        self.tension_v = (trafo.u_sec_v if trafo and self.config_fases != "1F+N"
                          else TENSION_POR_FASES[self.config_fases])

        for c in self.cargas:
            c.calcular(dg, fases_equipos.get(c.tag_emer) if c.tag_emer else None)
        for s in self.subcuadros:
            s.calcular(dg, self.zona_efectiva, self.trafo_efectivo, fases_equipos)

        self.p_inst_kw = sum(c.p_nom_kw for c in self.cargas) + sum(s.p_inst_kw for s in self.subcuadros)
        self.p_cargas_kw = sum(c.n_kw for c in self.cargas)
        self.q_cargas_kvar = sum(c.q_abs_kvar for c in self.cargas)
        self.p_kw = sum(c.p_kw for c in self.cargas) + sum(s.p_kw for s in self.subcuadros)
        self.q_kvar = (sum(c.q_kvar for c in self.cargas) + sum(s.q_kvar for s in self.subcuadros)
                       + sum(b.q_kvar for b in self.baterias))
        self.s_kva = math.hypot(self.p_kw, self.q_kvar)
        self.cos_phi = self.p_kw / self.s_kva if self.s_kva > EPS else 1.0

        # Ks global = P demandada / (P instalada con rendimientos + P demandada de subcuadros)
        denom = self.p_cargas_kw + sum(s.p_kw for s in self.subcuadros)
        self.ks_global = self.p_kw / denom if denom > EPS else 1.0

        self.s_emer_kva = (sum(c.s_emer_kva for c in self.cargas)
                           + sum(s.s_emer_kva for s in self.subcuadros)) * self.ke

        if self.calc_con_trafo and trafo:
            self.s_dimension_kva = trafo.s_adm_kva
        else:
            self.s_dimension_kva = self.s_kva
        if self.cos_phi < 0.9 and self.s_kva > EPS:
            self.avisos.append(f"cos φ bajo ({self.cos_phi:.3f}); revisar compensación")

    # ── recorridos ───────────────────────────────────────────────────────────
    def todos_los_cuadros(self) -> Iterator["Cuadro"]:
        yield self
        for s in self.subcuadros:
            yield from s.todos_los_cuadros()

    # ── serialización ────────────────────────────────────────────────────────
    def to_dict(self) -> dict:
        return {"rev": self.rev, "tag": self.tag, "nombre": self.nombre, "tipo": self.tipo,
                "config_fases": self.config_fases, "zona": self.zona, "trafo": self.trafo,
                "calc_con_trafo": self.calc_con_trafo, "tag_emer": self.tag_emer, "ke": self.ke,
                "cargas": [c.to_dict() for c in self.cargas],
                "baterias": [b.to_dict() for b in self.baterias],
                "subcuadros": [s.to_dict() for s in self.subcuadros]}

    @classmethod
    def from_dict(cls, d: dict) -> "Cuadro":
        c = cls(d.get("tag", ""), d.get("nombre", ""), tipo=d.get("tipo", "GENERAL BT"),
                config_fases=d.get("config_fases", "3F+N"), zona=d.get("zona", ""),
                trafo=d.get("trafo", ""), calc_con_trafo=bool(d.get("calc_con_trafo", False)),
                tag_emer=d.get("tag_emer"), ke=float(d.get("ke", 1.0)), rev=d.get("rev", ""))
        c.cargas = [Carga.from_dict(x) for x in d.get("cargas", [])]
        c.baterias = [BateriaCondensadores.from_dict(x) for x in d.get("baterias", [])]
        c.subcuadros = [Cuadro.from_dict(x) for x in d.get("subcuadros", [])]
        return c


class EquipoEmergencia(Elemento):
    """Grupo electrógeno o SAI, dimensionado con las cargas que lo tienen asociado."""

    def __init__(self, tipo: str, *, tag: str = "", nombre: str = "", rev: str = "",
                 forzar_trifasico: bool = False, horas: float = 1.0,
                 v_deposito_incl_l: float = 0.0, consumo_l_h: float = 0.0,
                 eficiencia_pct: float = 99.0):
        super().__init__(tag, nombre, rev)
        if tipo not in TIPOS_EMERGENCIA:
            raise ValueError(f"Tipo de equipo de emergencia desconocido: {tipo!r}")
        self.tipo = tipo
        self.forzar_trifasico = forzar_trifasico
        self.horas = horas
        self.v_deposito_incl_l = v_deposito_incl_l   # sólo G. ELEC.
        self.consumo_l_h = consumo_l_h               # sólo G. ELEC.
        self.eficiencia_pct = eficiencia_pct         # sólo UPS
        self._reset()

    def _reset(self):
        self.s_emer_kva = 0.0
        self.fases = ""
        self.tension_v = 0.0
        self.s_esp_min_kva = 0.0
        self.s_diseno_kva: Optional[float] = None
        self.v_deposito_ad_l: Optional[float] = None
        self.cap_bateria_kwh: Optional[float] = None
        self.n_consumidores = 0
        self.avisos: list = []

    def calcular(self, dg: DatosGenerales, s_emer_kva: float, n_consumidores: int) -> None:
        self._reset()
        self.s_emer_kva = s_emer_kva
        self.n_consumidores = n_consumidores
        self.fases = fases_automaticas(s_emer_kva, self.forzar_trifasico, dg.p_min_1f_3f_kw)
        self.tension_v = TENSION_POR_FASES[self.fases]
        self.s_esp_min_kva = s_emer_kva * FACTOR_SESP

        if self.tipo == "G. ELEC.":
            self.s_diseno_kva = siguiente_valor(s_emer_kva, LUT_GENERADORES)
            necesario = self.consumo_l_h * self.horas
            self.v_deposito_ad_l = max(0.0, necesario - self.v_deposito_incl_l) * MARGEN_DEPOSITO
        else:  # UPS
            tabla = LUT_SAI_1F if self.fases == "1F+N" else LUT_SAI_3F
            self.s_diseno_kva = siguiente_valor(s_emer_kva, tabla)
            if self.eficiencia_pct > 0:
                self.cap_bateria_kwh = s_emer_kva * self.horas / (self.eficiencia_pct / 100.0)
        if self.s_diseno_kva is None and s_emer_kva > 0:
            self.avisos.append("S de emergencia por encima de la tabla de equipos")
        if n_consumidores == 0:
            self.avisos.append("ninguna carga ni cuadro tiene asociado este equipo")

    def to_dict(self) -> dict:
        return {"rev": self.rev, "tag": self.tag, "nombre": self.nombre, "tipo": self.tipo,
                "forzar_trifasico": self.forzar_trifasico, "horas": self.horas,
                "v_deposito_incl_l": self.v_deposito_incl_l, "consumo_l_h": self.consumo_l_h,
                "eficiencia_pct": self.eficiencia_pct}

    @classmethod
    def from_dict(cls, d: dict) -> "EquipoEmergencia":
        return cls(d["tipo"], tag=d.get("tag", ""), nombre=d.get("nombre", ""),
                   rev=d.get("rev", ""), forzar_trifasico=bool(d.get("forzar_trifasico", False)),
                   horas=float(d.get("horas", 1.0)),
                   v_deposito_incl_l=float(d.get("v_deposito_incl_l", 0.0)),
                   consumo_l_h=float(d.get("consumo_l_h", 0.0)),
                   eficiencia_pct=float(d.get("eficiencia_pct", 99.0)))


class Proyecto:
    """Raíz del documento: condiciones generales + cuadros + equipos de emergencia."""

    def __init__(self):
        self.datos = DatosGenerales()
        self.cuadros: list[Cuadro] = []
        self.equipos: list[EquipoEmergencia] = []
        self.avisos: list = []
        self.ocupacion_trafos: dict = {}

    # ── cálculo global ───────────────────────────────────────────────────────
    def recalcular(self) -> None:
        if self.datos.tags_automaticos:
            self._asignar_tags()
        # Dos pasadas: las cargas heredan las fases del equipo de emergencia, y éste se
        # dimensiona con las cargas (la 2.ª pasada ya usa las fases de la 1.ª).
        fases_equipos: dict = {}
        for _ in range(2):
            for c in self.cuadros:
                c.calcular(self.datos, fases_equipos=fases_equipos)
            for e in self.equipos:
                s, n = self._consumo_emergencia(e.tag)
                e.calcular(self.datos, s, n)
            fases_equipos = {e.tag: e.fases for e in self.equipos}
        self._ocupacion_trafos()
        self.avisos = self._recopilar_avisos()

    def _asignar_tags(self) -> None:
        cont: dict = {}

        def sig(zona: str, pref: str) -> str:
            cont[(zona, pref)] = cont.get((zona, pref), 0) + 1
            n = cont[(zona, pref)]
            return f"{zona}-{pref}{n}" if zona else f"{pref}{n}"

        def visita(cuadro: Cuadro, zona_padre: str):
            zona = cuadro.zona or zona_padre
            for c in cuadro.cargas:
                c.tag = sig(zona, TIPOS_CARGA[c.tipo][0])
            for b in cuadro.baterias:
                b.tag = sig(zona, TAG_BATERIA)
            for s in cuadro.subcuadros:
                visita(s, zona)

        for c in self.cuadros:
            visita(c, "")

    def _consumo_emergencia(self, tag: str) -> tuple:
        total, n = 0.0, 0

        def visita(cuadro: Cuadro):
            nonlocal total, n
            if cuadro.tag_emer == tag:          # cuadro completo asociado: no se baja más
                total += cuadro.s_emer_kva
                n += 1
                return
            for c in cuadro.cargas:
                if c.tag_emer == tag:
                    total += c.s_emer_kva
                    n += 1
            for s in cuadro.subcuadros:
                visita(s)

        for c in self.cuadros:
            visita(c)
        return total, n

    def _ocupacion_trafos(self) -> None:
        acum: dict = {}
        for c in self.cuadros:
            if c.trafo_efectivo:
                p, q = acum.get(c.trafo_efectivo, (0.0, 0.0))
                acum[c.trafo_efectivo] = (p + c.p_kw, q + c.q_kvar)
        self.ocupacion_trafos = {}
        for tag, (p, q) in acum.items():
            t = self.datos.trafo(tag)
            s = math.hypot(p, q)
            self.ocupacion_trafos[tag] = (p, q, s, t.s_adm_kva if t else None)

    def _recopilar_avisos(self) -> list:
        av = []
        tags_emer = {e.tag for e in self.equipos}
        zonas = {z.tag for z in self.datos.zonas}
        for top in self.cuadros:
            for cu in top.todos_los_cuadros():
                for a in cu.avisos:
                    av.append(f"Cuadro {cu.tag}: {a}")
                if self.datos.zonas and not cu.zona_efectiva:
                    av.append(f"Cuadro {cu.tag}: sin zona asociada")
                elif cu.zona_efectiva and cu.zona_efectiva not in zonas and self.datos.zonas:
                    av.append(f"Cuadro {cu.tag}: zona {cu.zona_efectiva!r} no definida")
                if cu.trafo and self.datos.trafo(cu.trafo) is None:
                    av.append(f"Cuadro {cu.tag}: transformador {cu.trafo!r} no definido")
                if cu.tag_emer and cu.tag_emer not in tags_emer:
                    av.append(f"Cuadro {cu.tag}: equipo de emergencia {cu.tag_emer!r} no existe")
                for c in cu.cargas:
                    for a in c.avisos:
                        av.append(f"Carga {c.tag}: {a}")
                    if c.tag_emer and c.tag_emer not in tags_emer:
                        av.append(f"Carga {c.tag}: equipo de emergencia {c.tag_emer!r} no existe")
        for e in self.equipos:
            for a in e.avisos:
                av.append(f"Equipo {e.tag}: {a}")
        for tag, (_, _, s, adm) in self.ocupacion_trafos.items():
            if adm and s > adm:
                av.append(f"Transformador {tag}: S demandada {s:.1f} kVA > S admisible {adm:.1f} kVA")
        return av

    # ── utilidades ───────────────────────────────────────────────────────────
    def todos_los_cuadros(self) -> Iterator[Cuadro]:
        for c in self.cuadros:
            yield from c.todos_los_cuadros()

    def siguiente_tag_equipo(self, tipo: str) -> str:
        pref = TIPOS_EMERGENCIA[tipo][0]
        usados = {e.tag for e in self.equipos}
        n = 1
        while f"{pref}{n}" in usados:
            n += 1
        return f"{pref}{n}"

    def desasociar_emergencia(self, tag: str) -> None:
        for cu in self.todos_los_cuadros():
            if cu.tag_emer == tag:
                cu.tag_emer = None
            for c in cu.cargas:
                if c.tag_emer == tag:
                    c.tag_emer = None

    # ── serialización ────────────────────────────────────────────────────────
    def to_dict(self) -> dict:
        return {"formato": FORMATO, "version": VERSION,
                "datos_generales": self.datos.to_dict(),
                "cuadros": [c.to_dict() for c in self.cuadros],
                "equipos_emergencia": [e.to_dict() for e in self.equipos]}

    @classmethod
    def from_dict(cls, d: dict) -> "Proyecto":
        if d.get("formato") != FORMATO:
            raise ValueError("El fichero no es una lista de cargas (formato desconocido).")
        if int(d.get("version", 0)) > VERSION:
            raise ValueError("El fichero es de una versión más reciente del programa.")
        p = cls()
        p.datos = DatosGenerales.from_dict(d.get("datos_generales", {}))
        p.cuadros = [Cuadro.from_dict(c) for c in d.get("cuadros", [])]
        p.equipos = [EquipoEmergencia.from_dict(e) for e in d.get("equipos_emergencia", [])]
        return p


# ═════════════════════════════════════════════════════════════════════════════
#  PERSISTENCIA (*.list)
# ═════════════════════════════════════════════════════════════════════════════

def normalizar_ruta(ruta: str) -> str:
    ruta = os.path.expanduser(ruta)
    return ruta if ruta.lower().endswith(EXTENSION) else ruta + EXTENSION


def guardar(proyecto: Proyecto, ruta: str) -> None:
    """Escritura atómica: fichero temporal + reemplazo."""
    proyecto.recalcular()          # fija los tags automáticos antes de escribir
    ruta = normalizar_ruta(ruta)
    carpeta = os.path.dirname(os.path.abspath(ruta))
    fd, tmp = tempfile.mkstemp(dir=carpeta, prefix=".lista_", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(proyecto.to_dict(), f, ensure_ascii=False, indent=2)
            f.write("\n")
        os.replace(tmp, ruta)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def abrir(ruta: str) -> Proyecto:
    with open(normalizar_ruta(ruta), "r", encoding="utf-8") as f:
        return Proyecto.from_dict(json.load(f))


# ═════════════════════════════════════════════════════════════════════════════
#  PRESENTACIÓN
# ═════════════════════════════════════════════════════════════════════════════

def num(x: Optional[float], dec: int = 2) -> str:
    return "-" if x is None else f"{x:.{dec}f}"


def tabla(cabeceras: list, filas: list, sangria: str = "  ") -> str:
    filas = [[str(c) for c in f] for f in filas]
    anchos = [len(h) for h in cabeceras]
    for f in filas:
        for i, c in enumerate(f):
            anchos[i] = max(anchos[i], len(c))
    linea = lambda celdas: sangria + "  ".join(c.ljust(anchos[i]) for i, c in enumerate(celdas))
    salida = [linea(cabeceras), sangria + "  ".join("─" * a for a in anchos)]
    salida += [linea(f) for f in filas]
    return "\n".join(salida)


def titulo(texto: str, char: str = "═") -> None:
    print(f"\n{char * 4} {texto} {char * max(4, 76 - len(texto))}")


def mostrar_cuadro(c: Cuadro, nivel: int = 0) -> None:
    ind = "  " * nivel
    titulo(f"{ind}CUADRO {c.tag} · {c.nombre}")
    print(f"  Tipo: {c.tipo} | Fases: {c.config_fases} | U: {num(c.tension_v, 0)} V | "
          f"Zona: {c.zona_efectiva or '-'} | Trafo: {c.trafo_efectivo or '-'}"
          f"{' (calc. con trafo)' if c.calc_con_trafo else ''}")
    print(f"  P inst.: {num(c.p_inst_kw)} kW | P cargas: {num(c.p_cargas_kw)} kW | "
          f"Q cargas: {num(c.q_cargas_kvar)} kVAr | Ks global: {num(c.ks_global, 3)}")
    print(f"  DEMANDA → P: {num(c.p_kw)} kW | Q: {num(c.q_kvar)} kVAr | "
          f"S: {num(c.s_kva)} kVA | cos φ: {num(c.cos_phi, 3)} | S emer.: {num(c.s_emer_kva)} kVA")
    if c.calc_con_trafo:
        print(f"  S de dimensionado (trafo): {num(c.s_dimension_kva)} kVA")
    if c.cargas:
        print("\n  CARGAS")
        print(tabla(
            ["#", "TAG", "NOMBRE", "TIPO", "EMER", "FASES", "U[V]", "Pnom", "cosφ", "η%",
             "Ku", "Ks", "Ke", "P[kW]", "Q[kVAr]", "S[kVA]", "SEMER"],
            [[i + 1, x.tag, x.nombre, x.tipo, x.tag_emer or "-", x.fases, num(x.tension_v, 0),
              num(x.p_nom_kw), num(x.cos_efectivo, 3), num(x.eta_pct, 1), num(x.ku), num(x.ks),
              num(x.ke), num(x.p_kw), num(x.q_kvar), num(x.s_kva), num(x.s_emer_kva)]
             for i, x in enumerate(c.cargas)]))
    if c.subcuadros:
        print("\n  SUBCUADROS")
        print(tabla(
            ["#", "TAG", "NOMBRE", "TIPO", "EMER", "FASES", "U[V]", "Pinst", "cosφ", "Ks",
             "Ke", "P[kW]", "Q[kVAr]", "S[kVA]", "SEMER"],
            [[i + 1, s.tag, s.nombre, s.tipo, s.tag_emer or "-", s.config_fases,
              num(s.tension_v, 0), num(s.p_inst_kw), num(s.cos_phi, 3), num(s.ks_global, 3),
              num(s.ke), num(s.p_kw), num(s.q_kvar), num(s.s_kva), num(s.s_emer_kva)]
             for i, s in enumerate(c.subcuadros)]))
    if c.baterias:
        print("\n  BATERÍAS DE CONDENSADORES")
        print(tabla(["#", "TAG", "NOMBRE", "FASES", "U[V]", "Q[kVAr]"],
                    [[i + 1, b.tag, b.nombre, b.config_fases, num(b.tension_v, 0), num(b.q_kvar)]
                     for i, b in enumerate(c.baterias)]))
    for a in c.avisos:
        print(f"  ⚠ {a}")


def mostrar_arbol(proy: Proyecto) -> None:
    def rec(c: Cuadro, nivel: int):
        print(f"  {'   ' * nivel}{'└─ ' if nivel else ''}{c.tag or '(sin tag)'}  {c.nombre}  "
              f"[{len(c.cargas)} cargas, {len(c.baterias)} bat., {len(c.subcuadros)} subc.]  "
              f"P={num(c.p_kw)} kW  S={num(c.s_kva)} kVA")
        for s in c.subcuadros:
            rec(s, nivel + 1)

    if not proy.cuadros:
        print("  (no hay cuadros)")
    for i, c in enumerate(proy.cuadros):
        print(f"  [{i + 1}]", end="")
        rec(c, 0)


def mostrar_resumen_cargas(proy: Proyecto) -> None:
    """Equivalente a la hoja «RESUMEN DE CARGAS»."""
    titulo("RESUMEN DE CARGAS")
    for cu in proy.todos_los_cuadros():
        print(f"\n  {cu.nombre.upper() or cu.tag}  ({cu.tag})")
        filas = [[x.tag, x.nombre or "-", x.fases, num(x.tension_v, 0), num(x.p_nom_kw),
                  num(x.cos_efectivo, 3), num(x.ku), num(x.ks)] for x in cu.cargas]
        if filas:
            print(tabla(["TAG", "NOMBRE", "CONF. FASES", "U NOM [V]", "P ACTIVA NOM [kW]",
                         "cos φ", "Ku", "Ks"], filas))
        else:
            print("  (sin cargas)")


def mostrar_emergencia(proy: Proyecto) -> None:
    titulo("DIMENSIONAMIENTO DE EQUIPOS DE EMERGENCIA")
    if not proy.equipos:
        print("  (no hay equipos de emergencia)")
        return
    print(tabla(
        ["TAG", "NOMBRE", "TIPO", "Nº CONS.", "FASES", "U[V]", "SEMER[kVA]", "SESP mín[kVA]",
         "S DISEÑO[kVA]", "HORAS", "V DEP.AD.[L]", "EFIC.[%]", "CAP.BAT.[kWh]"],
        [[e.tag, e.nombre, e.tipo, e.n_consumidores, e.fases, num(e.tension_v, 0),
          num(e.s_emer_kva), num(e.s_esp_min_kva), num(e.s_diseno_kva, 1), num(e.horas, 1),
          num(e.v_deposito_ad_l, 0), num(e.eficiencia_pct, 1) if e.tipo == "UPS" else "-",
          num(e.cap_bateria_kwh)] for e in proy.equipos]))


def mostrar_resumen_completo(proy: Proyecto) -> None:
    proy.recalcular()
    for c in proy.cuadros:
        for cu in c.todos_los_cuadros():
            mostrar_cuadro(cu)
    mostrar_resumen_cargas(proy)
    if proy.ocupacion_trafos:
        titulo("OCUPACIÓN DE TRANSFORMADORES")
        print(tabla(["TRAFO", "P[kW]", "Q[kVAr]", "S[kVA]", "S ADM[kVA]", "OCUPACIÓN"],
                    [[t, num(p), num(q), num(s), num(a), f"{s / a * 100:.1f} %" if a else "-"]
                     for t, (p, q, s, a) in proy.ocupacion_trafos.items()]))
    mostrar_emergencia(proy)
    if proy.avisos:
        titulo("AVISOS")
        for a in proy.avisos:
            print(f"  ⚠ {a}")


# ═════════════════════════════════════════════════════════════════════════════
#  ENTRADA DE DATOS
# ═════════════════════════════════════════════════════════════════════════════

class Cancelado(Exception):
    """Ctrl+C durante una pregunta: se aborta la acción en curso."""


def entrada(prompt: str) -> str:
    try:
        return input(prompt).strip()
    except KeyboardInterrupt:
        print()
        raise Cancelado()
    except EOFError:
        print("\nFin de la entrada.")
        raise SystemExit(1)


def pedir_texto(msg: str, defecto: str = "", obligatorio: bool = False) -> str:
    while True:
        suf = f" [{defecto}]" if defecto else ""
        r = entrada(f"  {msg}{suf}: ")
        if r == "" and defecto != "":
            return defecto
        if r == "-" and not obligatorio:
            return ""
        if r or not obligatorio:
            return r
        print("    Este campo es obligatorio.")


def a_float(texto: str) -> float:
    return float(texto.replace(",", "."))


def pedir_float(msg: str, defecto: Optional[float] = None, minimo: Optional[float] = None,
                maximo: Optional[float] = None, exclusivo_min: bool = False) -> float:
    while True:
        suf = f" [{defecto:g}]" if defecto is not None else ""
        r = entrada(f"  {msg}{suf}: ")
        if r == "" and defecto is not None:
            return defecto
        try:
            v = a_float(r)
        except ValueError:
            print("    Introduce un número.")
            continue
        if minimo is not None and (v < minimo or (exclusivo_min and v <= minimo)):
            print(f"    Debe ser {'>' if exclusivo_min else '>='} {minimo:g}.")
            continue
        if maximo is not None and v > maximo:
            print(f"    Debe ser <= {maximo:g}.")
            continue
        return v


def pedir_float_o_auto(msg: str, actual: Optional[float], minimo: float, maximo: float,
                       exclusivo_min: bool = False) -> Optional[float]:
    """Enter = mantener, 'auto' = automático (tabla), número = valor manual."""
    while True:
        est = "auto" if actual is None else f"{actual:g}"
        r = entrada(f"  {msg} [{est}] (número, 'auto' o Enter para mantener): ")
        if r == "":
            return actual
        if r.lower() == "auto":
            return None
        try:
            v = a_float(r)
        except ValueError:
            print("    Introduce un número o 'auto'.")
            continue
        if v < minimo or (exclusivo_min and v <= minimo) or v > maximo:
            print(f"    Fuera de rango ({minimo:g}..{maximo:g}).")
            continue
        return v


def pedir_bool(msg: str, defecto: bool = False) -> bool:
    while True:
        r = entrada(f"  {msg} ({'S/n' if defecto else 's/N'}): ").lower()
        if r == "":
            return defecto
        if r in ("s", "si", "sí", "y", "yes"):
            return True
        if r in ("n", "no"):
            return False
        print("    Responde s o n.")


def pedir_opcion(msg: str, opciones: list, defecto: Optional[str] = None,
                 descripciones: Optional[dict] = None) -> str:
    print(f"  {msg}")
    for i, o in enumerate(opciones, 1):
        d = f"  - {descripciones[o]}" if descripciones and o in descripciones else ""
        marca = "  (actual)" if o == defecto else ""
        print(f"    {i:>2}) {o}{d}{marca}")
    while True:
        r = entrada("  Opción" + (" [Enter = actual]" if defecto else "") + ": ")
        if r == "" and defecto is not None:
            return defecto
        if r.isdigit() and 1 <= int(r) <= len(opciones):
            return opciones[int(r) - 1]
        if r in opciones:
            return r
        print("    Opción no válida.")


def pedir_indice(msg: str, n: int) -> Optional[int]:
    if n == 0:
        print("  (lista vacía)")
        return None
    r = entrada(f"  {msg} (1-{n}, Enter = cancelar): ")
    if r.isdigit() and 1 <= int(r) <= n:
        return int(r) - 1
    return None


def menu(titulo_menu: str, opciones: list, cabecera: Optional[Callable[[], None]] = None,
         texto_volver: str = "Volver") -> None:
    """Bucle de menú. opciones = [(texto, función)]; '0' siempre vuelve."""
    while True:
        titulo(titulo_menu)
        if cabecera:
            cabecera()
        for i, (txt, _) in enumerate(opciones, 1):
            print(f"  {i}) {txt}")
        print(f"  0) {texto_volver}")
        r = entrada("  > ")
        if r == "0":
            return
        if r.isdigit() and 1 <= int(r) <= len(opciones):
            try:
                if opciones[int(r) - 1][1]() == "SALIR":
                    return
            except Cancelado:
                print("  Operación cancelada.")
        else:
            print("  Opción no válida.")


# ═════════════════════════════════════════════════════════════════════════════
#  APLICACIÓN (menús)
# ═════════════════════════════════════════════════════════════════════════════

class App:
    def __init__(self, proyecto: Optional[Proyecto] = None, ruta: Optional[str] = None):
        self.proy = proyecto or Proyecto()
        self.ruta = ruta
        self._instantanea = self._estado()

    # ── estado / guardado ────────────────────────────────────────────────────
    def _estado(self) -> str:
        self.proy.recalcular()
        return json.dumps(self.proy.to_dict(), sort_keys=True)

    @property
    def modificado(self) -> bool:
        return self._estado() != self._instantanea

    def guardar(self, como: bool = False) -> bool:
        ruta = self.ruta
        if como or not ruta:
            ruta = pedir_texto("Nombre del fichero (.list)", self.ruta or "lista_cargas.list",
                               obligatorio=True)
        try:
            guardar(self.proy, ruta)
        except OSError as e:
            print(f"  ✗ No se pudo guardar: {e}")
            return False
        self.ruta = normalizar_ruta(ruta)
        self._instantanea = self._estado()
        print(f"  ✓ Guardado en {self.ruta}")
        return True

    def abrir(self) -> None:
        if self.modificado and not pedir_bool("Hay cambios sin guardar. ¿Descartarlos?", False):
            return
        ruta = pedir_texto("Fichero a abrir", obligatorio=True)
        try:
            self.proy = abrir(ruta)
        except (OSError, ValueError, KeyError, TypeError) as e:
            print(f"  ✗ No se pudo abrir: {e}")
            return
        self.ruta = normalizar_ruta(ruta)
        self._instantanea = self._estado()
        print(f"  ✓ Abierto {self.ruta}")

    # ── menú principal ───────────────────────────────────────────────────────
    def ejecutar(self, primera_vez: bool) -> None:
        print("\n  LISTA DE CARGAS ELÉCTRICAS")
        if primera_vez:
            print("  Proyecto nuevo: empezamos por las condiciones generales.")
            try:
                self.menu_condiciones()
            except Cancelado:
                print("  Operación cancelada.")

        def cab():
            self.proy.recalcular()
            print(f"  Fichero: {self.ruta or '(sin guardar)'}{'  *modificado*' if self.modificado else ''}")
            if self.proy.avisos:
                print(f"  ⚠ {len(self.proy.avisos)} aviso(s): ver «Resumen y cálculos»")

        opciones = [
            ("Condiciones generales", self.menu_condiciones),
            ("Cuadros eléctricos", self.menu_cuadros),
            ("Equipos de emergencia", self.menu_emergencia),
            ("Resumen y cálculos", self.menu_resumen),
            ("Guardar", lambda: self.guardar()),
            ("Guardar como…", lambda: self.guardar(como=True)),
            ("Abrir…", self.abrir),
        ]
        while True:
            menu("MENÚ PRINCIPAL", opciones, cab, texto_volver="Salir")
            if self.confirmar_salida():
                return

    def confirmar_salida(self) -> bool:
        if not self.modificado:
            return True
        r = entrada("  Hay cambios sin guardar. (g)uardar y salir / (s)alir sin guardar / (c)ancelar: ").lower()
        if r.startswith("g"):
            return self.guardar()
        return r.startswith("s")

    # ── 1. condiciones generales ─────────────────────────────────────────────
    def menu_condiciones(self) -> None:
        dg = self.proy.datos

        def cab():
            print(f"  Tags automáticos: {'SÍ' if dg.tags_automaticos else 'NO'} | "
                  f"cos φ/η automáticos (tablas): {'SÍ' if dg.tablas_automaticas else 'NO'} | "
                  f"P mín. 1F→3F: {dg.p_min_1f_3f_kw:g} kW")
            print(f"  Transformadores: {len(dg.transformadores)} | Zonas: {len(dg.zonas)}")

        def tags():
            dg.tags_automaticos = pedir_bool(
                "¿Tags automáticos? (si no, los tags de cargas se escriben a mano)", dg.tags_automaticos)

        def tablas():
            dg.tablas_automaticas = pedir_bool(
                "¿cos φ y rendimiento automáticos según tablas?", dg.tablas_automaticas)

        def pmin():
            dg.p_min_1f_3f_kw = pedir_float("P mín. de 1F a 3F [kW]", dg.p_min_1f_3f_kw, 0)

        menu("CONDICIONES GENERALES", [
            ("Activar/desactivar tags automáticos", tags),
            ("Activar/desactivar cos φ y η automáticos", tablas),
            ("P mín. 1F a 3F [kW]", pmin),
            ("Transformadores", self.menu_trafos),
            ("Zonas", self.menu_zonas),
            ("Datos de portada", self.editar_portada),
        ], cab)

    def editar_portada(self) -> None:
        p = self.proy.datos.portada
        p.empresa = pedir_texto("Empresa / ingeniería", p.empresa)
        p.codigo_documento = pedir_texto("Código del documento", p.codigo_documento)
        p.titulo = pedir_texto("Título", p.titulo)
        p.escrito = pedir_texto("Escrito por", p.escrito)
        p.comprobado = pedir_texto("Comprobado por", p.comprobado)
        p.verificado = pedir_texto("Verificado por", p.verificado)

    def menu_trafos(self) -> None:
        dg = self.proy.datos
        ts = dg.transformadores

        def cab():
            if ts:
                print(tabla(["#", "TAG", "SNOM [kVA]", "SADM [kVA]", "USEC [V]"],
                            [[i + 1, t.tag, num(t.s_nom_kva), num(t.s_adm_kva), num(t.u_sec_v, 0)]
                             for i, t in enumerate(ts)]))
            else:
                print("  (sin transformadores)")

        def anadir():
            tag = pedir_texto("TAG", f"TR{len(ts) + 1}", True)
            if dg.trafo(tag):
                print("    Ya existe un transformador con ese TAG.")
                return
            s = pedir_float("SNOM [kVA]", None, 0, exclusivo_min=True)
            ts.append(Transformador(tag, s, pedir_float("SADM [kVA]", s, 0, exclusivo_min=True),
                                    pedir_float("U secundario [V]", 400.0, 0, exclusivo_min=True)))

        def editar():
            i = pedir_indice("Transformador a editar", len(ts))
            if i is None:
                return
            t = ts[i]
            t.s_nom_kva = pedir_float("SNOM [kVA]", t.s_nom_kva, 0, exclusivo_min=True)
            t.s_adm_kva = pedir_float("SADM [kVA]", t.s_adm_kva, 0, exclusivo_min=True)
            t.u_sec_v = pedir_float("U secundario [V]", t.u_sec_v, 0, exclusivo_min=True)

        def borrar():
            i = pedir_indice("Transformador a eliminar", len(ts))
            if i is not None and pedir_bool(f"¿Eliminar {ts[i].tag}?"):
                tag = ts.pop(i).tag
                for cu in self.proy.todos_los_cuadros():
                    if cu.trafo == tag:
                        cu.trafo = ""

        menu("TRANSFORMADORES", [("Añadir", anadir), ("Editar", editar), ("Eliminar", borrar)], cab)

    def menu_zonas(self) -> None:
        zs = self.proy.datos.zonas

        def cab():
            if zs:
                print(tabla(["ZONA", "TAG", "DESCRIPCIÓN"],
                            [[i + 1, z.tag, z.descripcion] for i, z in enumerate(zs)]))
            else:
                print("  (sin zonas)")

        def anadir():
            tag = pedir_texto("TAG de zona (p. ej. PS)", obligatorio=True)
            if any(z.tag == tag for z in zs):
                print("    Ya existe esa zona.")
                return
            zs.append(Zona(tag, pedir_texto("Descripción")))

        def editar():
            i = pedir_indice("Zona a editar", len(zs))
            if i is not None:
                zs[i].descripcion = pedir_texto("Descripción", zs[i].descripcion)

        def borrar():
            i = pedir_indice("Zona a eliminar", len(zs))
            if i is not None and pedir_bool(f"¿Eliminar {zs[i].tag}?"):
                tag = zs.pop(i).tag
                for cu in self.proy.todos_los_cuadros():
                    if cu.zona == tag:
                        cu.zona = ""

        menu("ZONAS", [("Añadir", anadir), ("Editar descripción", editar), ("Eliminar", borrar)], cab)

    # ── 2. cuadros ───────────────────────────────────────────────────────────
    def menu_cuadros(self) -> None:
        cuadros = self.proy.cuadros

        def anadir():
            c = self.crear_cuadro(None)
            if c:
                cuadros.append(c)
                print(f"  ✓ Cuadro {c.tag} añadido.")

        def entrar():
            i = pedir_indice("Cuadro a abrir", len(cuadros))
            if i is not None:
                self.menu_cuadro(cuadros[i], cuadros)

        def borrar():
            i = pedir_indice("Cuadro a eliminar", len(cuadros))
            if i is not None and pedir_bool(f"¿Eliminar {cuadros[i].tag} con todo su contenido?"):
                self.eliminar_cuadro(cuadros, i)

        menu("CUADROS ELÉCTRICOS",
             [("Añadir cuadro", anadir), ("Abrir cuadro", entrar), ("Eliminar cuadro", borrar)],
             lambda: (self.proy.recalcular(), mostrar_arbol(self.proy)))

    def eliminar_cuadro(self, lista: list, i: int) -> None:
        cu = lista.pop(i)
        # si algún equipo de emergencia dependía de él no hace falta limpiar nada: el tag
        # está en las cargas/cuadros que se eliminan junto con él.
        print(f"  ✓ Cuadro {cu.tag} eliminado.")

    def crear_cuadro(self, padre: Optional[Cuadro]) -> Optional[Cuadro]:
        titulo("NUEVO SUBCUADRO" if padre else "NUEVO CUADRO", "─")
        tipo = pedir_opcion("Tipo de cuadro:", list(TIPOS_CUADRO), "GENERAL BT" if not padre else "DIST. SEC.",
                            {k: v[1] for k, v in TIPOS_CUADRO.items()})
        existentes = {c.tag for c in self.proy.todos_los_cuadros()}
        pref = TIPOS_CUADRO[tipo][0]
        propuesto, n = pref, 1
        while propuesto in existentes:
            n += 1
            propuesto = f"{pref}{n}"
        tag = pedir_texto("TAG del cuadro", propuesto, True)
        if tag in existentes:
            print("    Ya existe un cuadro con ese TAG.")
            return None
        c = Cuadro(tag, pedir_texto("Nombre", TIPOS_CUADRO[tipo][1]), tipo=tipo)
        self.editar_campos_cuadro(c, padre, nuevo=True)
        return c

    def editar_campos_cuadro(self, c: Cuadro, padre: Optional[Cuadro], nuevo: bool = False) -> None:
        dg = self.proy.datos
        c.rev = pedir_texto("Revisión", c.rev)
        c.config_fases = pedir_opcion("Configuración de fases:", list(CONFIG_FASES), c.config_fases)
        if dg.zonas:
            op = ["(heredar)" if padre else "(ninguna)"] + [z.tag for z in dg.zonas]
            z = pedir_opcion("Zona asociada:", op, c.zona or op[0],
                             {z.tag: z.descripcion for z in dg.zonas})
            c.zona = "" if z == op[0] else z
        if dg.transformadores:
            op = ["(heredar)" if padre else "(ninguno)"] + [t.tag for t in dg.transformadores]
            t = pedir_opcion("Transformador asociado:", op, c.trafo or op[0])
            c.trafo = "" if t == op[0] else t
            c.calc_con_trafo = pedir_bool("¿Dimensionar con la potencia admisible del trafo?",
                                          c.calc_con_trafo)
        if self.proy.equipos:
            c.tag_emer = self.pedir_equipo_emergencia(c.tag_emer)
        c.ke = pedir_float("Factor de emergencia Ke", c.ke, 0)

    def pedir_equipo_emergencia(self, actual: Optional[str]) -> Optional[str]:
        op = ["-"] + [e.tag for e in self.proy.equipos]
        r = pedir_opcion("Equipo de emergencia asociado:", op, actual or "-",
                         {e.tag: f"{e.nombre} ({e.tipo})" for e in self.proy.equipos})
        return None if r == "-" else r

    def menu_cuadro(self, c: Cuadro, lista_padre: list) -> None:
        padre_de_c = None
        for cu in self.proy.todos_los_cuadros():
            if c in cu.subcuadros:
                padre_de_c = cu
                break

        def cab():
            self.proy.recalcular()
            mostrar_cuadro(c)

        def editar():
            c.tag = pedir_texto("TAG", c.tag, True)
            c.nombre = pedir_texto("Nombre", c.nombre)
            c.tipo = pedir_opcion("Tipo de cuadro:", list(TIPOS_CUADRO), c.tipo,
                                  {k: v[1] for k, v in TIPOS_CUADRO.items()})
            self.editar_campos_cuadro(c, padre_de_c)

        def anadir_subcuadro():
            s = self.crear_cuadro(c)
            if s:
                c.subcuadros.append(s)
                print(f"  ✓ Subcuadro {s.tag} añadido.")

        def entrar_subcuadro():
            i = pedir_indice("Subcuadro a abrir", len(c.subcuadros))
            if i is not None:
                self.menu_cuadro(c.subcuadros[i], c.subcuadros)

        def borrar_subcuadro():
            i = pedir_indice("Subcuadro a eliminar", len(c.subcuadros))
            if i is not None and pedir_bool(f"¿Eliminar {c.subcuadros[i].tag} con todo su contenido?"):
                self.eliminar_cuadro(c.subcuadros, i)

        def anadir_bat():
            b = BateriaCondensadores(pedir_float("Q de la batería [kVAr]", None, 0, exclusivo_min=True))
            b.nombre = pedir_texto("Nombre")
            b.config_fases = pedir_opcion("Configuración de fases:", list(CONFIG_FASES), "3F")
            if not self.proy.datos.tags_automaticos:
                b.tag = pedir_texto("TAG", obligatorio=True)
            c.baterias.append(b)

        def editar_bat():
            i = pedir_indice("Batería a editar", len(c.baterias))
            if i is None:
                return
            b = c.baterias[i]
            if not self.proy.datos.tags_automaticos:
                b.tag = pedir_texto("TAG", b.tag, True)
            b.rev = pedir_texto("Revisión", b.rev)
            b.nombre = pedir_texto("Nombre", b.nombre)
            b.q_kvar_nominal = pedir_float("Q [kVAr]", b.q_kvar_nominal, 0, exclusivo_min=True)
            b.config_fases = pedir_opcion("Configuración de fases:", list(CONFIG_FASES), b.config_fases)

        def borrar_bat():
            i = pedir_indice("Batería a eliminar", len(c.baterias))
            if i is not None and pedir_bool(f"¿Eliminar {c.baterias[i].tag}?"):
                c.baterias.pop(i)

        opciones = [
            ("Editar datos del cuadro", editar),
            ("Añadir carga", lambda: self.anadir_carga(c)),
            ("Editar carga", lambda: self.editar_carga(c)),
            ("Eliminar carga", lambda: self.borrar_carga(c)),
            ("Duplicar carga", lambda: self.duplicar_carga(c)),
            ("Añadir subcuadro", anadir_subcuadro),
            ("Abrir subcuadro", entrar_subcuadro),
            ("Eliminar subcuadro", borrar_subcuadro),
            ("Añadir batería de condensadores", anadir_bat),
            ("Editar batería de condensadores", editar_bat),
            ("Eliminar batería de condensadores", borrar_bat),
        ]
        menu(f"CUADRO {c.tag}", opciones, cab)

    # ── cargas ───────────────────────────────────────────────────────────────
    def anadir_carga(self, cuadro: Cuadro) -> None:
        titulo("NUEVA CARGA", "─")
        tipo = pedir_opcion("Tipo de carga:", list(TIPOS_CARGA), "MOTOR",
                            {k: v[1] for k, v in TIPOS_CARGA.items()})
        carga = Carga(tipo, 0.0)
        carga.p_nom_kw = pedir_float("P activa nominal [kW]", None, 0)
        self.editar_campos_carga(carga, nueva=True)
        cuadro.cargas.append(carga)
        self.proy.recalcular()
        print(f"  ✓ Carga {carga.tag or '(sin tag)'} añadida.")

    def editar_campos_carga(self, c: Carga, nueva: bool = False) -> None:
        dg = self.proy.datos
        if not dg.tags_automaticos:
            c.tag = pedir_texto("TAG", c.tag, True)
        c.nombre = pedir_texto("Nombre", c.nombre)
        c.rev = pedir_texto("Revisión", c.rev)
        if not nueva:
            c.p_nom_kw = pedir_float("P activa nominal [kW]", c.p_nom_kw, 0)
        op = ["auto (las del equipo de emergencia asociado; si no, 3F+N)"] + list(CONFIG_FASES)
        r = pedir_opcion("Configuración de fases:", op, c.config_fases or op[0])
        c.config_fases = None if r == op[0] else r
        fijo = TIPOS_CARGA[c.tipo][2]
        if dg.tablas_automaticas:
            c.cos_phi = pedir_float_o_auto("cos φ", c.cos_phi, 0, 1, exclusivo_min=True)
            if c.tipo in TIPOS_CON_RENDIMIENTO or c.tipo == "M+VARIADOR":
                c.rendimiento_pct = pedir_float_o_auto("Rendimiento η [%]", c.rendimiento_pct, 0, 100,
                                                       exclusivo_min=True)
        else:
            c.cos_phi = pedir_float("cos φ", c.cos_phi if c.cos_phi is not None else
                                    (fijo if fijo is not None else 0.85), 0, 1, exclusivo_min=True)
            if c.tipo in TIPOS_CON_RENDIMIENTO:
                c.rendimiento_pct = pedir_float("Rendimiento η [%]",
                                                c.rendimiento_pct if c.rendimiento_pct is not None else 90.0,
                                                0, 100, exclusivo_min=True)
        c.ku = pedir_float("Factor de uso Ku", c.ku, 0, 1)
        c.ks = pedir_float("Factor de simultaneidad Ks", c.ks, 0, 1)
        c.ke = pedir_float("Factor de emergencia Ke", c.ke, 0, 1)
        if self.proy.equipos:
            c.tag_emer = self.pedir_equipo_emergencia(c.tag_emer)

    def editar_carga(self, cuadro: Cuadro) -> None:
        i = pedir_indice("Carga a editar", len(cuadro.cargas))
        if i is not None:
            self.editar_campos_carga(cuadro.cargas[i])

    def borrar_carga(self, cuadro: Cuadro) -> None:
        i = pedir_indice("Carga a eliminar", len(cuadro.cargas))
        if i is not None and pedir_bool(f"¿Eliminar {cuadro.cargas[i].tag}?"):
            cuadro.cargas.pop(i)

    def duplicar_carga(self, cuadro: Cuadro) -> None:
        i = pedir_indice("Carga a duplicar", len(cuadro.cargas))
        if i is None:
            return
        n = int(pedir_float("Número de copias", 1, 1, 500))
        for _ in range(n):
            d = Carga.from_dict(cuadro.cargas[i].to_dict())
            if not self.proy.datos.tags_automaticos:
                d.tag = pedir_texto(f"TAG de la copia (original {cuadro.cargas[i].tag})", obligatorio=True)
            cuadro.cargas.insert(i + 1, d)

    # ── 3. emergencia ────────────────────────────────────────────────────────
    def menu_emergencia(self) -> None:
        eqs = self.proy.equipos

        def anadir():
            tipo = pedir_opcion("Tipo de equipo:", list(TIPOS_EMERGENCIA), "G. ELEC.",
                                {k: v[1] for k, v in TIPOS_EMERGENCIA.items()})
            e = EquipoEmergencia(tipo, tag=self.proy.siguiente_tag_equipo(tipo))
            e.tag = pedir_texto("TAG", e.tag, True)
            if any(x.tag == e.tag for x in eqs):
                print("    Ya existe un equipo con ese TAG.")
                return
            self.editar_campos_equipo(e)
            eqs.append(e)

        def editar():
            i = pedir_indice("Equipo a editar", len(eqs))
            if i is not None:
                self.editar_campos_equipo(eqs[i])

        def borrar():
            i = pedir_indice("Equipo a eliminar", len(eqs))
            if i is not None and pedir_bool(f"¿Eliminar {eqs[i].tag}? (se desasociará de cargas y cuadros)"):
                self.proy.desasociar_emergencia(eqs.pop(i).tag)

        menu("EQUIPOS DE EMERGENCIA",
             [("Añadir equipo", anadir), ("Editar equipo", editar), ("Eliminar equipo", borrar)],
             lambda: (self.proy.recalcular(), mostrar_emergencia(self.proy)))

    def editar_campos_equipo(self, e: EquipoEmergencia) -> None:
        e.nombre = pedir_texto("Nombre", e.nombre)
        e.rev = pedir_texto("Revisión", e.rev)
        e.forzar_trifasico = pedir_bool("¿Forzar trifásico?", e.forzar_trifasico)
        e.horas = pedir_float("Nº de horas requeridas", e.horas, 0, exclusivo_min=True)
        if e.tipo == "G. ELEC.":
            e.v_deposito_incl_l = pedir_float("Volumen de depósito incluido [L]", e.v_deposito_incl_l, 0)
            e.consumo_l_h = pedir_float("Consumo [L/h]", e.consumo_l_h, 0)
        else:
            e.eficiencia_pct = pedir_float("Eficiencia [%]", e.eficiencia_pct, 0, 100, exclusivo_min=True)

    # ── 4. resumen ───────────────────────────────────────────────────────────
    def menu_resumen(self) -> None:
        self.proy.recalcular()
        mostrar_resumen_completo(self.proy)
        entrada("\n  [Enter para volver] ")


# ═════════════════════════════════════════════════════════════════════════════
#  MAIN
# ═════════════════════════════════════════════════════════════════════════════

def main(argv: Optional[list] = None) -> int:
    ap = argparse.ArgumentParser(description="Lista de cargas eléctricas (ficheros *.list)")
    ap.add_argument("archivo", nargs="?", help="fichero .list a abrir (o crear)")
    ap.add_argument("--resumen", action="store_true",
                    help="no interactivo: imprime los cálculos del fichero y sale")
    ap.add_argument("--gui", action="store_true",
                    help="abre la interfaz gráfica (lista_cargas_gui.py)")
    args = ap.parse_args(argv)

    if args.gui:
        try:
            import lista_cargas_gui
        except SystemExit:
            raise
        except ImportError as e:
            print(f"No se pudo cargar la interfaz gráfica: {e}", file=sys.stderr)
            return 1
        return lista_cargas_gui.main([args.archivo] if args.archivo else [])

    proyecto, ruta, nuevo = None, None, True
    if args.archivo:
        ruta = normalizar_ruta(args.archivo)
        if os.path.exists(ruta):
            try:
                proyecto, nuevo = abrir(ruta), False
            except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as e:
                print(f"No se pudo abrir {ruta}: {e}", file=sys.stderr)
                return 1
        elif args.resumen:
            print(f"No existe {ruta}", file=sys.stderr)
            return 1

    if args.resumen:
        if proyecto is None:
            print("--resumen necesita un fichero .list existente", file=sys.stderr)
            return 1
        mostrar_resumen_completo(proyecto)
        return 0

    app = App(proyecto, ruta)
    try:
        app.ejecutar(primera_vez=nuevo)
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
