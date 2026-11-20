#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
lista_cargas_gui.py - Interfaz gráfica (tkinter) de la lista de cargas eléctricas.

Funciona igual en Windows y Linux (sólo biblioteca estándar). Necesita que
`lista_cargas.py` esté en la misma carpeta: de ahí salen el modelo, las tablas y
todos los cálculos, y el formato de fichero *.list es exactamente el mismo que el
de la versión de consola.

Uso:
    python lista_cargas_gui.py [fichero.list]

Requisito en Linux (si tkinter no está instalado):
    Debian/Ubuntu:  sudo apt install python3-tk
    Fedora:         sudo dnf install python3-tkinter
    Arch:           sudo pacman -S tk
"""
from __future__ import annotations

import json
import os
import sys
import traceback
from dataclasses import dataclass
from typing import Callable, Optional

try:
    import tkinter as tk
    import tkinter.font as tkfont
    from tkinter import filedialog, messagebox, ttk
except ImportError:  # pragma: no cover
    sys.exit("No se encuentra tkinter.\n"
             "  Linux (Debian/Ubuntu): sudo apt install python3-tk\n"
             "  Linux (Fedora):        sudo dnf install python3-tkinter\n"
             "  Windows: reinstala Python marcando «tcl/tk and IDLE».")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import lista_cargas as lc
except ImportError:  # pragma: no cover
    sys.exit("No se encuentra lista_cargas.py; debe estar en la misma carpeta que este fichero.")

NOMBRE_APP = "Lista de cargas"
TIPOS_FICHERO = [("Lista de cargas (*.list)", "*.list"), ("Todos los ficheros", "*.*")]
ROJO = "#c00000"
NO_APLICA = "-"


# ═════════════════════════════════════════════════════════════════════════════
#  UTILIDADES DE FORMATO Y VALIDACIÓN
# ═════════════════════════════════════════════════════════════════════════════

def t(x, dec: int = 2) -> str:
    """Número para mostrar (coma decimal, como en el Excel)."""
    return NO_APLICA if x is None else f"{x:.{dec}f}".replace(".", ",")


def edit_num(x) -> str:
    """Número para un campo editable."""
    return "" if x is None else ("%.10g" % x).replace(".", ",")


def parse_float(raw: str, nombre: str) -> float:
    try:
        return float(str(raw).strip().replace(",", "."))
    except ValueError:
        raise ValueError(f"{nombre}: introduce un número") from None


def rango(v: float, nombre: str, minimo=None, maximo=None, excl: bool = False) -> float:
    if minimo is not None and (v < minimo or (excl and v <= minimo)):
        raise ValueError(f"{nombre}: debe ser {'>' if excl else '≥'} {edit_num(minimo)}")
    if maximo is not None and v > maximo:
        raise ValueError(f"{nombre}: debe ser ≤ {edit_num(maximo)}")
    return v


def f_num(attr: str, nombre: str, minimo=None, maximo=None, excl: bool = False):
    def get(o):
        return edit_num(getattr(o, attr))

    def set_(o, raw):
        setattr(o, attr, rango(parse_float(raw, nombre), nombre, minimo, maximo, excl))
    return get, set_


def f_str(attr: str, nombre: str = "", obligatorio: bool = False):
    def get(o):
        return getattr(o, attr) or ""

    def set_(o, raw):
        raw = raw.strip()
        if obligatorio and not raw:
            raise ValueError(f"{nombre}: obligatorio")
        setattr(o, attr, raw)
    return get, set_


def f_opcional(attr: str, nombre: str, maximo: float):
    """Número en (0, maximo] o «auto» (None → se toma de las tablas)."""
    def get(o):
        v = getattr(o, attr)
        return "auto" if v is None else edit_num(v)

    def set_(o, raw):
        r = str(raw).strip().lower()
        if r in ("", "auto"):
            setattr(o, attr, None)
        else:
            setattr(o, attr, rango(parse_float(r, nombre), nombre, 0, maximo, True))
    return get, set_


# ═════════════════════════════════════════════════════════════════════════════
#  FORMULARIO GENÉRICO (edición «en vivo» de un objeto del modelo)
# ═════════════════════════════════════════════════════════════════════════════

@dataclass
class Campo:
    etiqueta: str
    get: Callable                       # obj -> str | bool
    set: Callable                       # (obj, valor) -> None; lanza ValueError si no vale
    tipo: str = "texto"                 # texto | combo | check
    valores: Optional[Callable] = None  # obj -> lista (sólo combo)
    ancho: int = 22
    activo: Optional[Callable] = None   # obj -> bool (False = sólo lectura)
    commit: bool = False                # aplicar sólo al salir del campo / Intro
    reconstruir: bool = False           # tras cambiar, volver a dibujar el formulario


class FormView(ttk.Frame):
    """Formulario con una columna de datos editables y otra de resultados."""

    def __init__(self, master, al_cambiar: Callable[[bool], None]):
        super().__init__(master)
        self.al_cambiar = al_cambiar
        self.obj = None
        self._filas: list = []          # (Campo, Variable, widget)
        self._res: list = []            # (funcion, Label)
        self._cargando = False
        self.columnconfigure(0, weight=0)
        self.columnconfigure(1, weight=1)
        self.lf_datos = ttk.LabelFrame(self, text="Datos", padding=6)
        self.lf_datos.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        self.lf_res = ttk.LabelFrame(self, text="Resultados", padding=6)
        self.lf_res.grid(row=0, column=1, sticky="nsew")
        self.lbl_error = ttk.Label(self, foreground=ROJO, wraplength=620)
        self.lbl_error.grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 0))

    # ── construcción ─────────────────────────────────────────────────────────
    def cargar(self, obj, campos: list, resultados: list, titulo: str = "Datos") -> None:
        self.obj = obj
        for w in self.lf_datos.winfo_children() + self.lf_res.winfo_children():
            w.destroy()
        self._filas.clear()
        self._res.clear()
        self.lbl_error.config(text="")
        self.lf_datos.config(text=titulo)

        self.lf_datos.columnconfigure(1, weight=1)
        for i, c in enumerate(campos):
            ttk.Label(self.lf_datos, text=c.etiqueta).grid(row=i, column=0, sticky="w", padx=(0, 8), pady=2)
            valor = c.get(obj)
            if c.tipo == "check":
                var = tk.BooleanVar(value=bool(valor))
                w = ttk.Checkbutton(self.lf_datos, variable=var,
                                    command=lambda c=c, v=var: self._aplicar(c, v.get(), None))
            elif c.tipo == "combo":
                var = tk.StringVar(value=valor)
                w = ttk.Combobox(self.lf_datos, textvariable=var, values=c.valores(obj),
                                 state="readonly", width=c.ancho)
                w.bind("<<ComboboxSelected>>", lambda e, c=c, v=var: self._aplicar(c, v.get(), None))
            else:
                var = tk.StringVar(value=valor)
                w = ttk.Entry(self.lf_datos, textvariable=var, width=c.ancho)
                if c.commit:
                    w.bind("<FocusOut>", lambda e, c=c, v=var, w=w: self._aplicar(c, v.get(), w))
                    w.bind("<Return>", lambda e, c=c, v=var, w=w: self._aplicar(c, v.get(), w))
                else:
                    var.trace_add("write", lambda *a, c=c, v=var, w=w: self._escrito(c, v, w))
            w.grid(row=i, column=1, sticky="ew", pady=2)
            self._filas.append((c, var, w))

        if resultados:
            self.lf_res.grid()
            self.lf_res.columnconfigure(1, weight=1)
            for i, (etq, fn) in enumerate(resultados):
                ttk.Label(self.lf_res, text=etq).grid(row=i, column=0, sticky="nw", padx=(0, 10), pady=1)
                lbl = ttk.Label(self.lf_res, text="", wraplength=240, justify="left")
                lbl.grid(row=i, column=1, sticky="nw", pady=1)
                self._res.append((fn, lbl))
        else:
            self.lf_res.grid_remove()
        self.actualizar()

    # ── edición ──────────────────────────────────────────────────────────────
    def _escrito(self, c: Campo, var, w) -> None:
        if not self._cargando:
            self._aplicar(c, var.get(), w)

    def _aplicar(self, c: Campo, valor, w) -> None:
        if self.obj is None or not self.winfo_exists():
            return
        try:
            if c.commit and c.get(self.obj) == valor:
                return
            c.set(self.obj, valor)
        except ValueError as e:
            self.lbl_error.config(text=str(e))
            if w is not None and w.winfo_exists():
                w.configure(style="Err.TEntry")
            return
        self.lbl_error.config(text="")
        if w is not None and w.winfo_exists():
            w.configure(style="TEntry")
        self.al_cambiar(c.reconstruir)

    # ── refresco (tras recalcular) ───────────────────────────────────────────
    def actualizar(self) -> None:
        if self.obj is None:
            return
        try:
            foco = self.focus_get()
        except (KeyError, tk.TclError):
            foco = None
        for c, var, w in self._filas:
            nuevo = c.get(self.obj)
            if c.tipo == "check":
                var.set(bool(nuevo))
            elif w is not foco and var.get() != nuevo:
                self._cargando = True
                try:
                    var.set(nuevo)
                finally:
                    self._cargando = False
                if c.tipo == "texto":
                    w.configure(style="TEntry")
            if c.tipo == "texto":
                editable = c.activo(self.obj) if c.activo else True
                w.configure(state="normal" if editable else "readonly")
        for fn, lbl in self._res:
            lbl.config(text=fn(self.obj))

    def vaciar(self) -> None:
        self.obj = None


# ═════════════════════════════════════════════════════════════════════════════
#  DIÁLOGO MODAL SENCILLO
# ═════════════════════════════════════════════════════════════════════════════

@dataclass
class DCampo:
    clave: str
    etiqueta: str
    tipo: str = "texto"          # texto | float | combo | check
    valores: Optional[list] = None
    defecto: object = ""
    obligatorio: bool = False
    minimo: Optional[float] = None
    maximo: Optional[float] = None
    excl: bool = False
    ayuda: str = ""


class Dialogo(tk.Toplevel):
    def __init__(self, master, titulo: str, campos: list, validar: Optional[Callable] = None):
        super().__init__(master)
        self.title(titulo)
        self.transient(master)
        self.resizable(False, False)
        self.resultado: Optional[dict] = None
        self.campos, self.validar = campos, validar
        self.vars: dict = {}

        cuerpo = ttk.Frame(self, padding=14)
        cuerpo.pack(fill="both", expand=True)
        cuerpo.columnconfigure(1, weight=1)
        primero = None
        for i, c in enumerate(campos):
            ttk.Label(cuerpo, text=c.etiqueta).grid(row=i, column=0, sticky="w", pady=3, padx=(0, 10))
            if c.tipo == "check":
                var = tk.BooleanVar(value=bool(c.defecto))
                w = ttk.Checkbutton(cuerpo, variable=var)
            elif c.tipo == "combo":
                var = tk.StringVar(value=str(c.defecto))
                w = ttk.Combobox(cuerpo, textvariable=var, values=c.valores, state="readonly", width=34)
            else:
                var = tk.StringVar(value="" if c.defecto is None else str(c.defecto))
                w = ttk.Entry(cuerpo, textvariable=var, width=36)
            w.grid(row=i, column=1, sticky="ew", pady=3)
            if c.ayuda:
                ttk.Label(cuerpo, text=c.ayuda, foreground="#666666").grid(row=i, column=2, sticky="w", padx=(8, 0))
            self.vars[c.clave] = var
            primero = primero or w
        n = len(campos)
        self.lbl_err = ttk.Label(cuerpo, foreground=ROJO, wraplength=420)
        self.lbl_err.grid(row=n, column=0, columnspan=3, sticky="w", pady=(6, 0))
        botones = ttk.Frame(cuerpo)
        botones.grid(row=n + 1, column=0, columnspan=3, sticky="e", pady=(10, 0))
        ttk.Button(botones, text="Aceptar", command=self.aceptar).pack(side="left", padx=4)
        ttk.Button(botones, text="Cancelar", command=self.destroy).pack(side="left")
        self.bind("<Return>", lambda e: self.aceptar())
        self.bind("<Escape>", lambda e: self.destroy())

        self.update_idletasks()
        x = master.winfo_rootx() + max(0, (master.winfo_width() - self.winfo_width()) // 2)
        y = master.winfo_rooty() + max(0, (master.winfo_height() - self.winfo_height()) // 3)
        self.geometry(f"+{x}+{y}")
        try:
            self.wait_visibility()
            self.grab_set()
        except tk.TclError:
            pass
        if primero is not None:
            primero.focus_set()

    def aceptar(self) -> None:
        res: dict = {}
        try:
            for c in self.campos:
                v = self.vars[c.clave].get()
                if c.tipo == "float":
                    if str(v).strip() == "":
                        if c.obligatorio:
                            raise ValueError(f"{c.etiqueta}: obligatorio")
                        res[c.clave] = None
                    else:
                        res[c.clave] = rango(parse_float(v, c.etiqueta), c.etiqueta, c.minimo, c.maximo, c.excl)
                elif c.tipo == "texto":
                    v = v.strip()
                    if c.obligatorio and not v:
                        raise ValueError(f"{c.etiqueta}: obligatorio")
                    res[c.clave] = v
                else:
                    res[c.clave] = v
            if self.validar:
                self.validar(res)
        except ValueError as e:
            self.lbl_err.config(text=str(e))
            return
        self.resultado = res
        self.destroy()

    @classmethod
    def pedir(cls, master, titulo: str, campos: list, validar: Optional[Callable] = None) -> Optional[dict]:
        d = cls(master, titulo, campos, validar)
        master.wait_window(d)
        return d.resultado


# ═════════════════════════════════════════════════════════════════════════════
#  TABLAS (Treeview con barras de desplazamiento)
# ═════════════════════════════════════════════════════════════════════════════

def crear_tabla(master, columnas: list, altura: int = 8, arbol: bool = False,
                ancho_arbol: int = 200, titulo_arbol: str = "", estirar_arbol: bool = True):
    """columnas = [(id, titulo, ancho, 'w'|'e'|'center')]. Devuelve (frame, tree)."""
    marco = ttk.Frame(master)
    marco.columnconfigure(0, weight=1)
    marco.rowconfigure(0, weight=1)
    tree = ttk.Treeview(marco, columns=[c[0] for c in columnas], height=altura,
                        show="tree headings" if arbol else "headings", selectmode="browse")
    if arbol:
        tree.heading("#0", text=titulo_arbol, anchor="w")
        tree.column("#0", width=ancho_arbol, minwidth=60, stretch=estirar_arbol)
    for cid, tit, ancho, anchor in columnas:
        tree.heading(cid, text=tit, anchor=anchor)
        tree.column(cid, width=ancho, minwidth=40, anchor=anchor, stretch=False)
    vs = ttk.Scrollbar(marco, orient="vertical", command=tree.yview)
    hs = ttk.Scrollbar(marco, orient="horizontal", command=tree.xview)
    tree.configure(yscrollcommand=vs.set, xscrollcommand=hs.set)
    tree.grid(row=0, column=0, sticky="nsew")
    vs.grid(row=0, column=1, sticky="ns")
    hs.grid(row=1, column=0, sticky="ew")
    return marco, tree


def sync_plano(tree, objs: list, fila: Callable, prefijo: str) -> None:
    """Actualiza un Treeview plano sin parpadeo (conserva la selección)."""
    deseados = set()
    for i, o in enumerate(objs):
        iid = f"{prefijo}{id(o)}"
        deseados.add(iid)
        texto, valores = fila(o)
        if tree.exists(iid):
            tree.move(iid, "", i)
            tree.item(iid, text=texto, values=valores)
        else:
            tree.insert("", i, iid=iid, text=texto, values=valores)
    for iid in tree.get_children(""):
        if iid not in deseados:
            tree.delete(iid)


def clave(o) -> str:
    return f"n{id(o)}"


# ═════════════════════════════════════════════════════════════════════════════
#  APLICACIÓN
# ═════════════════════════════════════════════════════════════════════════════

COLS_CONTENIDO = [("tag", "TAG", 90, "w"), ("nombre", "NOMBRE", 130, "w"), ("tipo", "TIPO", 90, "w"),
                  ("emer", "EMER.", 50, "w"), ("fases", "FASES", 50, "w"), ("u", "U [V]", 50, "e"),
                  ("p", "P nom/inst [kW]", 115, "e"), ("cos", "cos φ", 50, "e"), ("eta", "η [%]", 50, "e"),
                  ("ku", "Ku", 40, "e"), ("ks", "Ks", 50, "e"), ("ke", "Ke", 40, "e"),
                  ("pd", "P [kW]", 70, "e"), ("qd", "Q [kVAr]", 70, "e"), ("sd", "S [kVA]", 70, "e"),
                  ("se", "S EMER [kVA]", 85, "e")]


class AppGUI(tk.Tk):
    def __init__(self, ruta: Optional[str] = None):
        super().__init__()
        self.proy = lc.Proyecto()
        self.ruta: Optional[str] = None
        self._snap = ""
        self._job = None
        self._interno = False
        self._ultimo_tipo_carga = "MOTOR"
        self.report_callback_exception = self._excepcion

        self.geometry("1320x820")
        self.minsize(1000, 640)
        self._estilo()
        self._menu()

        self.nb = ttk.Notebook(self)
        self.nb.pack(fill="both", expand=True, padx=6, pady=(6, 0))
        self.tab_gen = ttk.Frame(self.nb, padding=8)
        self.tab_cua = ttk.Frame(self.nb, padding=4)
        self.tab_eme = ttk.Frame(self.nb, padding=4)
        self.tab_res = ttk.Frame(self.nb, padding=4)
        self.nb.add(self.tab_gen, text="1. Condiciones generales")
        self.nb.add(self.tab_cua, text="2. Cuadros eléctricos")
        self.nb.add(self.tab_eme, text="3. Equipos de emergencia")
        self.nb.add(self.tab_res, text="4. Resumen")
        self._build_general()
        self._build_cuadros()
        self._build_emergencia()
        self._build_resumen()
        self._build_barra()
        self.nb.bind("<<NotebookTabChanged>>", self._al_cambiar_pestana)

        self.bind_all("<Control-n>", lambda e: self.nuevo())
        self.bind_all("<Control-o>", lambda e: self.abrir())
        self.bind_all("<Control-s>", lambda e: self.guardar())
        self.bind_all("<Control-S>", lambda e: self.guardar(como=True))
        self.protocol("WM_DELETE_WINDOW", self.salir)

        if ruta and os.path.exists(lc.normalizar_ruta(ruta)):
            self._abrir_ruta(ruta)
        else:
            if ruta:
                self.ruta = lc.normalizar_ruta(ruta)   # fichero nuevo con ese nombre
            self._recargar_generales()
            self.refrescar_ahora()
            self._snap = self._instantanea()
            self._barra()

    # ── estilo ───────────────────────────────────────────────────────────────
    def _estilo(self) -> None:
        st = ttk.Style(self)
        for tema in ("vista", "xpnative", "clam"):
            if tema in st.theme_names():
                st.theme_use(tema)
                break
        st.configure("Err.TEntry", foreground=ROJO)
        fuente = tkfont.nametofont("TkDefaultFont")
        st.configure("Treeview", rowheight=max(20, int(fuente.metrics("linespace") * 1.45)))
        self.fuente_negrita = fuente.copy()
        self.fuente_negrita.configure(weight="bold")

    def _excepcion(self, exc, val, tb) -> None:
        detalle = "".join(traceback.format_exception(exc, val, tb))
        sys.stderr.write(detalle)
        messagebox.showerror("Error inesperado", f"{val}\n\n{detalle[-1500:]}")

    # ── menú ─────────────────────────────────────────────────────────────────
    def _menu(self) -> None:
        barra = tk.Menu(self)
        m = tk.Menu(barra, tearoff=0)
        m.add_command(label="Nuevo", accelerator="Ctrl+N", command=self.nuevo)
        m.add_command(label="Abrir…", accelerator="Ctrl+O", command=self.abrir)
        m.add_separator()
        m.add_command(label="Guardar", accelerator="Ctrl+S", command=self.guardar)
        m.add_command(label="Guardar como…", accelerator="Ctrl+Mayús+S", command=lambda: self.guardar(como=True))
        m.add_separator()
        m.add_command(label="Salir", command=self.salir)
        barra.add_cascade(label="Archivo", menu=m)
        a = tk.Menu(barra, tearoff=0)
        a.add_command(label="Acerca de…", command=lambda: messagebox.showinfo(
            NOMBRE_APP, "Lista de cargas eléctricas (BT)\n\nConversión a Python del Excel "
                        "«Lista de cargas».\nLos datos se guardan en ficheros *.list."))
        barra.add_cascade(label="Ayuda", menu=a)
        self.config(menu=barra)

    # ── barra de estado ──────────────────────────────────────────────────────
    def _build_barra(self) -> None:
        b = ttk.Frame(self, padding=(8, 2))
        b.pack(fill="x", side="bottom")
        self.lbl_estado = ttk.Label(b, text="")
        self.lbl_estado.pack(side="left")
        self.lbl_avisos = ttk.Label(b, text="", foreground=ROJO, cursor="hand2")
        self.lbl_avisos.pack(side="right")
        self.lbl_avisos.bind("<Button-1>", lambda e: self._ir_a_avisos())

    def _ir_a_avisos(self) -> None:
        self.nb.select(self.tab_res)
        self.nb_res.select(self.tab_avisos)

    # ═════════════════════════════════════════════════════════════════════════
    #  ESTADO, DEBOUNCE Y GUARDADO
    # ═════════════════════════════════════════════════════════════════════════
    def _instantanea(self) -> str:
        return json.dumps(self.proy.to_dict(), sort_keys=True)

    @property
    def modificado(self) -> bool:
        return self._instantanea() != self._snap

    def cambiado(self, reconstruir: bool = False, inmediato: bool = False) -> None:
        """Programa un recálculo (con pequeña espera para no recalcular en cada tecla)."""
        self._reconstruir = getattr(self, "_reconstruir", False) or reconstruir
        if self._job:
            self.after_cancel(self._job)
        if inmediato:
            self.refrescar_ahora()
        else:
            self._job = self.after(120, self.refrescar_ahora)

    def refrescar_ahora(self) -> None:
        if self._job:
            self.after_cancel(self._job)
            self._job = None
        reconstruir, self._reconstruir = getattr(self, "_reconstruir", False), False
        self.proy.recalcular()
        self._sync_arbol()
        self._refrescar_tabla()
        sync_plano(self.tree_eq, self.proy.equipos, self._fila_equipo, "e")
        if reconstruir:
            self._mostrar_seleccion()
            self._mostrar_equipo()
        else:
            if self.form.obj is not None:
                self.form.actualizar()
            if self.form_eq.obj is not None:
                self.form_eq.actualizar()
        if self.nb.select() == str(self.tab_res):
            self._refrescar_resumen()
        self._barra()

    def _barra(self) -> None:
        nombre = os.path.basename(self.ruta) if self.ruta else "(sin guardar)"
        mod = self.modificado
        self.title(f"{'*' if mod else ''}{nombre} — {NOMBRE_APP}")
        self.lbl_estado.config(text=f"{self.ruta or '(fichero nuevo, sin guardar)'}"
                                    f"{'   · modificado' if mod else ''}")
        n = len(self.proy.avisos)
        self.lbl_avisos.config(text=f"⚠ {n} aviso{'s' if n != 1 else ''}" if n else "")

    def _al_cambiar_pestana(self, _e=None) -> None:
        sel = self.nb.select()
        if sel == str(self.tab_cua):
            self._mostrar_seleccion()
        elif sel == str(self.tab_eme):
            self._mostrar_equipo()
        elif sel == str(self.tab_res):
            self._refrescar_resumen()

    # ── archivo ──────────────────────────────────────────────────────────────
    def _confirmar_descartar(self) -> bool:
        if not self.modificado:
            return True
        r = messagebox.askyesnocancel(NOMBRE_APP, "Hay cambios sin guardar. ¿Guardar antes de continuar?")
        if r is None:
            return False
        return self.guardar() if r else True

    def nuevo(self) -> None:
        if not self._confirmar_descartar():
            return
        self._cargar_proyecto(lc.Proyecto(), None)
        self.nb.select(self.tab_gen)

    def abrir(self) -> None:
        if not self._confirmar_descartar():
            return
        ruta = filedialog.askopenfilename(parent=self, title="Abrir lista de cargas", filetypes=TIPOS_FICHERO)
        if ruta:
            self._abrir_ruta(ruta)

    def _abrir_ruta(self, ruta: str) -> None:
        try:
            proy = lc.abrir(ruta)
        except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as e:
            messagebox.showerror(NOMBRE_APP, f"No se pudo abrir el fichero:\n{e}")
            return
        self._cargar_proyecto(proy, lc.normalizar_ruta(ruta))

    def _cargar_proyecto(self, proy, ruta: Optional[str]) -> None:
        self.proy, self.ruta = proy, ruta
        for tree in (self.tree, self.tree_eq):
            tree.delete(*tree.get_children(""))
        self.form.vaciar()
        self.form_eq.vaciar()
        self._recargar_generales()
        self.refrescar_ahora()
        self._snap = self._instantanea()
        primero = self.tree.get_children("")
        if primero:
            self.tree.selection_set(primero[0])
        self._mostrar_seleccion()
        self._mostrar_equipo()
        self._barra()

    def guardar(self, como: bool = False) -> bool:
        ruta = self.ruta
        if como or not ruta:
            ruta = filedialog.asksaveasfilename(
                parent=self, title="Guardar lista de cargas", defaultextension=lc.EXTENSION,
                filetypes=TIPOS_FICHERO, initialfile=os.path.basename(self.ruta) if self.ruta else "lista_cargas.list")
            if not ruta:
                return False
        self.refrescar_ahora()
        try:
            lc.guardar(self.proy, ruta)
        except OSError as e:
            messagebox.showerror(NOMBRE_APP, f"No se pudo guardar:\n{e}")
            return False
        self.ruta = lc.normalizar_ruta(ruta)
        self._snap = self._instantanea()
        self._barra()
        return True

    def salir(self) -> None:
        if self._confirmar_descartar():
            self.destroy()

    # ═════════════════════════════════════════════════════════════════════════
    #  1. CONDICIONES GENERALES
    # ═════════════════════════════════════════════════════════════════════════
    def _build_general(self) -> None:
        f = self.tab_gen
        f.columnconfigure(0, weight=1)
        f.columnconfigure(1, weight=1)
        f.rowconfigure(1, weight=1)

        self.form_cond = FormView(f, lambda rec: self.cambiado())
        self.form_cond.grid(row=0, column=0, sticky="new", padx=(0, 8))
        self.form_port = FormView(f, lambda rec: self.cambiado())
        self.form_port.grid(row=0, column=1, sticky="new")

        abajo = ttk.Frame(f)
        abajo.grid(row=1, column=0, columnspan=2, sticky="nsew", pady=(10, 0))
        abajo.columnconfigure(0, weight=1)
        abajo.columnconfigure(1, weight=1)
        abajo.rowconfigure(0, weight=1)

        # transformadores
        lf = ttk.LabelFrame(abajo, text="Datos de transformadores", padding=6)
        lf.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        lf.columnconfigure(0, weight=1)
        lf.rowconfigure(0, weight=1)
        marco, self.tree_tr = crear_tabla(lf, [("snom", "SNOM [kVA]", 105, "e"), ("sadm", "SADM [kVA]", 105, "e"),
                                               ("usec", "USECUNDARIO [V]", 135, "e")],
                                          arbol=True, ancho_arbol=90, titulo_arbol="TAG", estirar_arbol=False)
        marco.grid(row=0, column=0, sticky="nsew")
        bt = ttk.Frame(lf)
        bt.grid(row=1, column=0, sticky="w", pady=(6, 0))
        for txt, cmd in (("Añadir", self.trafo_anadir), ("Editar", self.trafo_editar), ("Eliminar", self.trafo_borrar)):
            ttk.Button(bt, text=txt, command=cmd).pack(side="left", padx=(0, 4))
        self.tree_tr.bind("<Double-1>", lambda e: self.trafo_editar())

        # zonas
        lf = ttk.LabelFrame(abajo, text="Definición de zonas", padding=6)
        lf.grid(row=0, column=1, sticky="nsew")
        lf.columnconfigure(0, weight=1)
        lf.rowconfigure(0, weight=1)
        marco, self.tree_zo = crear_tabla(lf, [("desc", "DESCRIPCIÓN", 320, "w")],
                                          arbol=True, ancho_arbol=90, titulo_arbol="TAG", estirar_arbol=False)
        marco.grid(row=0, column=0, sticky="nsew")
        bt = ttk.Frame(lf)
        bt.grid(row=1, column=0, sticky="w", pady=(6, 0))
        for txt, cmd in (("Añadir", self.zona_anadir), ("Editar", self.zona_editar), ("Eliminar", self.zona_borrar)):
            ttk.Button(bt, text=txt, command=cmd).pack(side="left", padx=(0, 4))
        self.tree_zo.bind("<Double-1>", lambda e: self.zona_editar())

    def _recargar_generales(self) -> None:
        dg = self.proy.datos
        g, s = f_num("p_min_1f_3f_kw", "P mín. 1F a 3F", 0)
        campos = [
            Campo("Tags automáticos", lambda o: o.tags_automaticos,
                  lambda o, v: setattr(o, "tags_automaticos", bool(v)), "check"),
            Campo("cos φ y η automáticos (tablas)", lambda o: o.tablas_automaticas,
                  lambda o, v: setattr(o, "tablas_automaticas", bool(v)), "check"),
            Campo("P mín. 1F a 3F [kW]", g, s, ancho=10),
        ]
        self.form_cond.cargar(dg, campos, [], "Condiciones de cálculo")
        p = dg.portada
        campos = [Campo(etq, *f_str(attr), ancho=34) for etq, attr in (
            ("Empresa / ingeniería", "empresa"), ("Código del documento", "codigo_documento"),
            ("Título", "titulo"), ("Escrito por", "escrito"), ("Comprobado por", "comprobado"),
            ("Verificado por", "verificado"))]
        self.form_port.cargar(p, campos, [], "Datos de portada")
        self._recargar_tablas_generales()

    def _recargar_tablas_generales(self) -> None:
        dg = self.proy.datos
        self.tree_tr.delete(*self.tree_tr.get_children(""))
        for tr in dg.transformadores:
            self.tree_tr.insert("", "end", iid=tr.tag, text=tr.tag,
                                values=(t(tr.s_nom_kva), t(tr.s_adm_kva), t(tr.u_sec_v, 0)))
        self.tree_zo.delete(*self.tree_zo.get_children(""))
        for z in dg.zonas:
            self.tree_zo.insert("", "end", iid=z.tag, text=z.tag, values=(z.descripcion,))

    def _tras_editar_listas(self) -> None:
        self._recargar_tablas_generales()
        self.cambiado(inmediato=True)

    # transformadores
    def _dlg_trafo(self, tr=None) -> Optional[dict]:
        dg = self.proy.datos

        def validar(r):
            if any(x.tag == r["tag"] and x is not tr for x in dg.transformadores):
                raise ValueError("Ya existe un transformador con ese TAG")
        campos = [DCampo("tag", "TAG", defecto=tr.tag if tr else f"TR{len(dg.transformadores) + 1}", obligatorio=True),
                  DCampo("snom", "SNOM [kVA]", "float", defecto=edit_num(tr.s_nom_kva) if tr else "",
                         obligatorio=True, minimo=0, excl=True),
                  DCampo("sadm", "SADM [kVA]", "float", defecto=edit_num(tr.s_adm_kva) if tr else "",
                         minimo=0, excl=True, ayuda="vacío = SNOM"),
                  DCampo("usec", "U secundario [V]", "float", defecto=edit_num(tr.u_sec_v) if tr else "400",
                         obligatorio=True, minimo=0, excl=True)]
        return Dialogo.pedir(self, "Transformador", campos, validar)

    def trafo_anadir(self) -> None:
        r = self._dlg_trafo()
        if r:
            self.proy.datos.transformadores.append(
                lc.Transformador(r["tag"], r["snom"], r["sadm"] or r["snom"], r["usec"]))
            self._tras_editar_listas()

    def trafo_editar(self) -> None:
        sel = self.tree_tr.selection()
        if not sel:
            return
        tr = self.proy.datos.trafo(sel[0])
        r = self._dlg_trafo(tr)
        if r and tr:
            if r["tag"] != tr.tag:
                for cu in self.proy.todos_los_cuadros():
                    if cu.trafo == tr.tag:
                        cu.trafo = r["tag"]
            tr.tag, tr.s_nom_kva, tr.s_adm_kva, tr.u_sec_v = r["tag"], r["snom"], r["sadm"] or r["snom"], r["usec"]
            self._tras_editar_listas()

    def trafo_borrar(self) -> None:
        sel = self.tree_tr.selection()
        if sel and messagebox.askyesno(NOMBRE_APP, f"¿Eliminar el transformador {sel[0]}?\n"
                                                    "Se quitará de los cuadros que lo usan."):
            self.proy.datos.transformadores = [x for x in self.proy.datos.transformadores if x.tag != sel[0]]
            for cu in self.proy.todos_los_cuadros():
                if cu.trafo == sel[0]:
                    cu.trafo = ""
            self._tras_editar_listas()
            self._mostrar_seleccion()

    # zonas
    def _dlg_zona(self, z=None) -> Optional[dict]:
        dg = self.proy.datos

        def validar(r):
            if any(x.tag == r["tag"] and x is not z for x in dg.zonas):
                raise ValueError("Ya existe una zona con ese TAG")
        campos = [DCampo("tag", "TAG (p. ej. PS)", defecto=z.tag if z else "", obligatorio=True),
                  DCampo("desc", "Descripción", defecto=z.descripcion if z else "")]
        return Dialogo.pedir(self, "Zona", campos, validar)

    def zona_anadir(self) -> None:
        r = self._dlg_zona()
        if r:
            self.proy.datos.zonas.append(lc.Zona(r["tag"], r["desc"]))
            self._tras_editar_listas()

    def zona_editar(self) -> None:
        sel = self.tree_zo.selection()
        if not sel:
            return
        z = next((x for x in self.proy.datos.zonas if x.tag == sel[0]), None)
        r = self._dlg_zona(z)
        if r and z:
            if r["tag"] != z.tag:
                for cu in self.proy.todos_los_cuadros():
                    if cu.zona == z.tag:
                        cu.zona = r["tag"]
            z.tag, z.descripcion = r["tag"], r["desc"]
            self._tras_editar_listas()

    def zona_borrar(self) -> None:
        sel = self.tree_zo.selection()
        if sel and messagebox.askyesno(NOMBRE_APP, f"¿Eliminar la zona {sel[0]}?\n"
                                                    "Se quitará de los cuadros que la usan."):
            self.proy.datos.zonas = [x for x in self.proy.datos.zonas if x.tag != sel[0]]
            for cu in self.proy.todos_los_cuadros():
                if cu.zona == sel[0]:
                    cu.zona = ""
            self._tras_editar_listas()
            self._mostrar_seleccion()

    # ═════════════════════════════════════════════════════════════════════════
    #  2. CUADROS ELÉCTRICOS
    # ═════════════════════════════════════════════════════════════════════════
    def _build_cuadros(self) -> None:
        pw = ttk.PanedWindow(self.tab_cua, orient="horizontal")
        pw.pack(fill="both", expand=True)

        # izquierda: árbol + botones
        izq = ttk.Frame(pw, padding=(0, 0, 6, 0))
        izq.columnconfigure(0, weight=1)
        izq.rowconfigure(1, weight=1)
        barra = ttk.Frame(izq)
        barra.grid(row=0, column=0, sticky="ew", pady=(0, 4))
        botones = [("+ Cuadro", self.añadir_cuadro), ("+ Subcuadro", self.añadir_subcuadro),
                   ("+ Carga", self.añadir_carga), ("+ Batería", self.añadir_bateria),
                   ("Duplicar", self.duplicar), ("Mover a…", self.mover),
                   ("Subir", lambda: self.reordenar(-1)), ("Bajar", lambda: self.reordenar(1)),
                   ("Eliminar", self.eliminar)]
        for i, (txt, cmd) in enumerate(botones):
            ttk.Button(barra, text=txt, command=cmd, width=11).grid(row=i // 3, column=i % 3, padx=2, pady=2, sticky="ew")
        for c in range(3):
            barra.columnconfigure(c, weight=1)

        marco, self.tree = crear_tabla(
            izq, [("tipo", "TIPO", 90, "w"), ("p", "P [kW]", 65, "e"), ("q", "Q [kVAr]", 70, "e"),
                  ("s", "S [kVA]", 65, "e")],
            altura=20, arbol=True, ancho_arbol=200, titulo_arbol="TAG · NOMBRE")
        marco.grid(row=1, column=0, sticky="nsew")
        self.tree.tag_configure("cuadro", font=self.fuente_negrita)
        self.tree.tag_configure("aviso", foreground=ROJO)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._mostrar_seleccion(forzar=False))
        self.tree.bind("<Delete>", lambda e: self.eliminar())
        for ev in ("<Button-3>", "<Button-2>"):
            self.tree.bind(ev, self._menu_contextual)
        self._nodos: dict = {}      # iid -> (objeto, tipo, padre)
        pw.add(izq, weight=1)
        self.after(150, lambda: pw.sashpos(0, 500))

        # derecha: formulario + tabla del cuadro
        der = ttk.Frame(pw)
        der.columnconfigure(0, weight=1)
        der.rowconfigure(2, weight=1)
        self.hint = ttk.Label(der, text="Añade un cuadro con «+ Cuadro» o selecciona un elemento del árbol.",
                              foreground="#666666", padding=20)
        self.hint.grid(row=0, column=0, sticky="nw")
        self.form = FormView(der, lambda rec: self.cambiado(rec))
        self.form.grid(row=0, column=0, sticky="new")
        self.form.grid_remove()
        self.lbl_tabla = ttk.Label(der, text="Contenido del cuadro", font=self.fuente_negrita)
        self.lbl_tabla.grid(row=1, column=0, sticky="w", pady=(10, 2))
        marco, self.tabla = crear_tabla(der, [(c[0], c[1], c[2], c[3]) for c in COLS_CONTENIDO], altura=8)
        marco.grid(row=2, column=0, sticky="nsew")
        self.tabla.bind("<<TreeviewSelect>>", self._seleccion_tabla)
        pw.add(der, weight=3)

        self.menu_ctx = tk.Menu(self, tearoff=0)
        for txt, cmd in (("Añadir subcuadro", self.añadir_subcuadro), ("Añadir carga", self.añadir_carga),
                         ("Añadir batería", self.añadir_bateria), (None, None), ("Duplicar", self.duplicar),
                         ("Mover a…", self.mover), (None, None), ("Eliminar", self.eliminar)):
            if txt:
                self.menu_ctx.add_command(label=txt, command=cmd)
            else:
                self.menu_ctx.add_separator()

    def _menu_contextual(self, ev) -> None:
        iid = self.tree.identify_row(ev.y)
        if iid:
            self.tree.selection_set(iid)
            self.menu_ctx.tk_popup(ev.x_root, ev.y_root)

    # ── árbol ────────────────────────────────────────────────────────────────
    def _fila_arbol(self, o, tipo: str) -> tuple:
        if tipo == "cuadro":
            return (f"{o.tag}  {o.nombre}".strip(), (o.tipo, t(o.p_kw), t(o.q_kvar), t(o.s_kva)),
                    ("cuadro", "aviso") if o.avisos else ("cuadro",))
        if tipo == "carga":
            return (f"{o.tag}  {o.nombre}".strip(), (o.tipo, t(o.p_kw), t(o.q_kvar), t(o.s_kva)),
                    ("aviso",) if o.avisos else ())
        return (f"{o.tag}  {o.nombre}".strip(), (lc.TIPO_BATERIA, NO_APLICA, t(o.q_kvar), NO_APLICA), ())

    def _sync_arbol(self) -> None:
        tree = self.tree
        deseados: set = set()
        nodos: dict = {}

        def hijos(parent_iid: str, lista: list) -> None:
            for i, (o, tipo, padre) in enumerate(lista):
                iid = clave(o)
                deseados.add(iid)
                nodos[iid] = (o, tipo, padre)
                texto, valores, tags = self._fila_arbol(o, tipo)
                if tree.exists(iid):
                    if tree.parent(iid) != parent_iid or tree.index(iid) != i:
                        tree.move(iid, parent_iid, i)
                    tree.item(iid, text=texto, values=valores, tags=tags)
                else:
                    tree.insert(parent_iid, i, iid=iid, text=texto, values=valores, tags=tags, open=True)
                if tipo == "cuadro":
                    hijos(iid, [(c, "carga", o) for c in o.cargas] + [(b, "bat", o) for b in o.baterias]
                          + [(s, "cuadro", o) for s in o.subcuadros])

        hijos("", [(c, "cuadro", None) for c in self.proy.cuadros])

        def recorrer(iid: str):
            for h in tree.get_children(iid):
                yield h
                yield from recorrer(h)
        for iid in list(recorrer("")):
            if iid not in deseados and tree.exists(iid):
                tree.delete(iid)
        self._nodos = nodos

    def _sel(self):
        s = self.tree.selection()
        return self._nodos.get(s[0]) if s else None

    def _cuadro_actual(self):
        sel = self._sel()
        if not sel:
            return None
        o, tipo, padre = sel
        return o if tipo == "cuadro" else padre

    def _padre_de(self, cuadro):
        for cu in self.proy.todos_los_cuadros():
            if cuadro in cu.subcuadros:
                return cu
        return None

    def _lista_de(self, o, tipo, padre) -> list:
        if tipo == "carga":
            return padre.cargas
        if tipo == "bat":
            return padre.baterias
        return padre.subcuadros if padre else self.proy.cuadros

    def _seleccionar(self, o) -> None:
        self.refrescar_ahora()
        iid = clave(o)
        if self.tree.exists(iid):
            self.tree.selection_set(iid)
            self.tree.see(iid)
        self._mostrar_seleccion()

    # ── tabla de contenido del cuadro ────────────────────────────────────────
    def _fila_tabla(self, o, tipo: str) -> tuple:
        if tipo == "carga":
            return (o.tag, o.nombre, o.tipo, o.tag_emer or NO_APLICA, o.fases, t(o.tension_v, 0), t(o.p_nom_kw),
                    t(o.cos_efectivo, 3), t(o.eta_pct, 1), t(o.ku), t(o.ks), t(o.ke), t(o.p_kw), t(o.q_kvar),
                    t(o.s_kva), t(o.s_emer_kva))
        if tipo == "cuadro":
            return (o.tag, o.nombre, "SUBCUADRO", o.tag_emer or NO_APLICA, o.config_fases, t(o.tension_v, 0),
                    t(o.p_inst_kw), t(o.cos_phi, 3), NO_APLICA, NO_APLICA, t(o.ks_global, 3), t(o.ke), t(o.p_kw),
                    t(o.q_kvar), t(o.s_kva), t(o.s_emer_kva))
        return (o.tag, o.nombre, lc.TIPO_BATERIA, NO_APLICA, o.config_fases, t(o.tension_v, 0), NO_APLICA, NO_APLICA,
                NO_APLICA, NO_APLICA, NO_APLICA, NO_APLICA, NO_APLICA, t(o.q_kvar), NO_APLICA, NO_APLICA)

    def _refrescar_tabla(self) -> None:
        cu = self._cuadro_actual()
        self.tabla.delete(*self.tabla.get_children(""))
        if cu is None:
            self.lbl_tabla.config(text="Contenido del cuadro")
            return
        self.lbl_tabla.config(text=f"Contenido del cuadro {cu.tag}")
        for o, tipo in ([(c, "carga") for c in cu.cargas] + [(s, "cuadro") for s in cu.subcuadros]
                        + [(b, "bat") for b in cu.baterias]):
            self.tabla.insert("", "end", iid="t" + clave(o), values=self._fila_tabla(o, tipo))
        sel = self.tree.selection()
        if sel and self.tabla.exists("t" + sel[0]) and self.tabla.selection() != ("t" + sel[0],):
            self._interno = True
            self.tabla.selection_set("t" + sel[0])
            self.after_idle(self._fin_interno)

    def _fin_interno(self) -> None:
        self._interno = False

    def _seleccion_tabla(self, _e=None) -> None:
        if self._interno:
            return
        s = self.tabla.selection()
        if s and self.tree.exists(s[0][1:]) and self.tree.selection() != (s[0][1:],):
            self.tree.selection_set(s[0][1:])
            self.tree.see(s[0][1:])

    # ── formularios ──────────────────────────────────────────────────────────
    def _mostrar_seleccion(self, forzar: bool = True) -> None:
        sel = self._sel()
        if not forzar and sel is not None and sel[0] is self.form.obj:
            self._refrescar_tabla()
            return
        if sel is None:
            self.form.vaciar()
            self.form.grid_remove()
            self.hint.grid()
            self._refrescar_tabla()
            return
        o, tipo, padre = sel
        self.hint.grid_remove()
        self.form.grid()
        if tipo == "cuadro":
            campos, res, tit = self._def_cuadro(o, padre)
        elif tipo == "carga":
            campos, res, tit = self._def_carga(o)
        else:
            campos, res, tit = self._def_bateria(o)
        self.form.cargar(o, campos, res, tit)
        self._refrescar_tabla()

    def _valores_equipos(self, _o=None) -> list:
        return [NO_APLICA] + [e.tag for e in self.proy.equipos]

    @staticmethod
    def _combo_str(attr: str, vacio: Optional[str] = None):
        """Combo cuyo valor «vacío» se guarda como '' / None según el atributo."""
        def get(o):
            return getattr(o, attr) or (vacio or "")

        def set_(o, raw):
            setattr(o, attr, None if raw == vacio and attr == "tag_emer" else ("" if raw == vacio else raw))
        return get, set_

    def _def_cuadro(self, c, padre):
        dg = self.proy.datos
        heredar = "(heredar)" if padre else "(ninguna)"

        def set_tag(o, raw):
            raw = raw.strip()
            if not raw:
                raise ValueError("TAG: obligatorio")
            if any(x is not o and x.tag == raw for x in self.proy.todos_los_cuadros()):
                raise ValueError("TAG: ya existe otro cuadro con ese TAG")
            o.tag = raw

        def lista_con_actual(base: list, actual: str):
            return lambda o: [heredar] + base + ([actual] if actual and actual not in base else [])

        gz, sz = self._combo_str("zona", heredar)
        gt, st = self._combo_str("trafo", heredar)
        ge, se = self._combo_str("tag_emer", NO_APLICA)
        gk, sk = f_num("ke", "Ke", 0)
        campos = [
            Campo("TAG", lambda o: o.tag, set_tag),
            Campo("Nombre", *f_str("nombre"), ancho=30),
            Campo("Revisión", *f_str("rev"), ancho=10),
            Campo("Tipo de cuadro", lambda o: o.tipo, lambda o, v: setattr(o, "tipo", v), "combo",
                  lambda o: list(lc.TIPOS_CUADRO)),
            Campo("Configuración de fases", lambda o: o.config_fases, lambda o, v: setattr(o, "config_fases", v),
                  "combo", lambda o: list(lc.CONFIG_FASES)),
            Campo("Zona asociada", gz, sz, "combo", lista_con_actual([z.tag for z in dg.zonas], c.zona)),
            Campo("Transformador asociado", gt, st, "combo",
                  lista_con_actual([x.tag for x in dg.transformadores], c.trafo)),
            Campo("Dimensionar con S adm. del trafo", lambda o: o.calc_con_trafo,
                  lambda o, v: setattr(o, "calc_con_trafo", bool(v)), "check"),
            Campo("Equipo de emergencia", ge, se, "combo", self._valores_equipos),
            Campo("Factor de emergencia Ke", gk, sk, ancho=10),
        ]
        res = [("Tensión [V]", lambda o: t(o.tension_v, 0)),
               ("Zona / trafo efectivos", lambda o: f"{o.zona_efectiva or '-'} / {o.trafo_efectivo or '-'}"),
               ("P instalada [kW]", lambda o: t(o.p_inst_kw)),
               ("P cargas absorbida [kW]", lambda o: t(o.p_cargas_kw)),
               ("Q cargas absorbida [kVAr]", lambda o: t(o.q_cargas_kvar)),
               ("Ks global", lambda o: t(o.ks_global, 3)),
               ("P demandada [kW]", lambda o: t(o.p_kw)),
               ("Q demandada [kVAr]", lambda o: t(o.q_kvar)),
               ("S demandada [kVA]", lambda o: t(o.s_kva)),
               ("cos φ", lambda o: t(o.cos_phi, 3)),
               ("S emergencia [kVA]", lambda o: t(o.s_emer_kva)),
               ("S dimensionado [kVA]", lambda o: t(o.s_dimension_kva)),
               ("Avisos", lambda o: "; ".join(o.avisos) or NO_APLICA)]
        return campos, res, f"{'Subcuadro' if padre else 'Cuadro'} {c.tag}"

    def _def_carga(self, c):
        dg = self.proy.datos
        ge, se = self._combo_str("tag_emer", NO_APLICA)

        def set_tag(o, raw):
            if not raw.strip():
                raise ValueError("TAG: obligatorio")
            o.tag = raw.strip()

        def set_fases(o, raw):
            o.config_fases = None if raw == "auto" else raw
        campos = [
            Campo("TAG" + (" (automático)" if dg.tags_automaticos else ""), lambda o: o.tag, set_tag,
                  activo=lambda o: not self.proy.datos.tags_automaticos),
            Campo("Nombre", *f_str("nombre"), ancho=30),
            Campo("Revisión", *f_str("rev"), ancho=10),
            Campo("Tipo de carga", lambda o: o.tipo, lambda o, v: setattr(o, "tipo", v), "combo",
                  lambda o: list(lc.TIPOS_CARGA), reconstruir=True),
            Campo("P activa nominal [kW]", *f_num("p_nom_kw", "P nominal", 0), ancho=12),
            Campo("Fases (auto = las del equipo)", lambda o: o.config_fases or "auto", set_fases, "combo",
                  lambda o: ["auto"] + list(lc.CONFIG_FASES)),
            Campo("cos φ (número o «auto»)", *f_opcional("cos_phi", "cos φ", 1), ancho=12),
        ]
        if c.tipo in lc.TIPOS_CON_RENDIMIENTO or c.tipo == "M+VARIADOR":
            campos.append(Campo("Rendimiento η [%] (o «auto»)", *f_opcional("rendimiento_pct", "η", 100), ancho=12))
        campos += [
            Campo("Factor de uso Ku", *f_num("ku", "Ku", 0, 1), ancho=12),
            Campo("Factor de simultaneidad Ks", *f_num("ks", "Ks", 0, 1), ancho=12),
            Campo("Factor de emergencia Ke", *f_num("ke", "Ke", 0, 1), ancho=12),
            Campo("Equipo de emergencia", ge, se, "combo", self._valores_equipos),
        ]
        res = [("Fases / tensión", lambda o: f"{o.fases} / {t(o.tension_v, 0)} V"),
               ("cos φ efectivo", lambda o: t(o.cos_efectivo, 3)),
               ("η efectivo [%]", lambda o: t(o.eta_pct, 1)),
               ("P absorbida [kW]", lambda o: t(o.n_kw)),
               ("Q absorbida [kVAr]", lambda o: t(o.q_abs_kvar)),
               ("P demandada [kW]", lambda o: t(o.p_kw)),
               ("Q demandada [kVAr]", lambda o: t(o.q_kvar)),
               ("S demandada [kVA]", lambda o: t(o.s_kva)),
               ("S emergencia [kVA]", lambda o: t(o.s_emer_kva)),
               ("Avisos", lambda o: "; ".join(o.avisos) or NO_APLICA)]
        return campos, res, f"Carga {c.tag}"

    def _def_bateria(self, b):
        def set_tag(o, raw):
            if not raw.strip():
                raise ValueError("TAG: obligatorio")
            o.tag = raw.strip()
        campos = [
            Campo("TAG" + (" (automático)" if self.proy.datos.tags_automaticos else ""), lambda o: o.tag, set_tag,
                  activo=lambda o: not self.proy.datos.tags_automaticos),
            Campo("Nombre", *f_str("nombre"), ancho=30),
            Campo("Revisión", *f_str("rev"), ancho=10),
            Campo("Q de la batería [kVAr]", *f_num("q_kvar_nominal", "Q", 0, None, True), ancho=12),
            Campo("Configuración de fases", lambda o: o.config_fases, lambda o, v: setattr(o, "config_fases", v),
                  "combo", lambda o: list(lc.CONFIG_FASES)),
        ]
        res = [("Tensión [V]", lambda o: t(o.tension_v, 0)),
               ("Q aportada [kVAr]", lambda o: t(o.q_kvar))]
        return campos, res, f"Batería de condensadores {b.tag}"

    # ── acciones sobre el árbol ──────────────────────────────────────────────
    def _tag_libre_cuadro(self, tipo: str) -> str:
        usados = {c.tag for c in self.proy.todos_los_cuadros()}
        pref = lc.TIPOS_CUADRO[tipo][0]
        cand, n = pref, 1
        while cand in usados:
            n += 1
            cand = f"{pref}{n}"
        return cand

    def _dlg_cuadro(self, subcuadro: bool) -> Optional[dict]:
        def validar(r):
            if r["tag"] and any(c.tag == r["tag"] for c in self.proy.todos_los_cuadros()):
                raise ValueError("Ya existe un cuadro con ese TAG")
        campos = [DCampo("tipo", "Tipo de cuadro", "combo", list(lc.TIPOS_CUADRO),
                         "DIST. SEC." if subcuadro else "GENERAL BT"),
                  DCampo("tag", "TAG", ayuda="vacío = automático"),
                  DCampo("nombre", "Nombre", ayuda="vacío = descripción del tipo")]
        r = Dialogo.pedir(self, "Nuevo subcuadro" if subcuadro else "Nuevo cuadro", campos, validar)
        if r:
            r["tag"] = r["tag"] or self._tag_libre_cuadro(r["tipo"])
            r["nombre"] = r["nombre"] or lc.TIPOS_CUADRO[r["tipo"]][1]
        return r

    def añadir_cuadro(self) -> None:
        r = self._dlg_cuadro(False)
        if r:
            c = lc.Cuadro(r["tag"], r["nombre"], tipo=r["tipo"])
            self.proy.cuadros.append(c)
            self._seleccionar(c)

    def añadir_subcuadro(self) -> None:
        cu = self._cuadro_actual()
        if cu is None:
            messagebox.showinfo(NOMBRE_APP, "Selecciona primero el cuadro donde añadir el subcuadro.")
            return
        r = self._dlg_cuadro(True)
        if r:
            s = lc.Cuadro(r["tag"], r["nombre"], tipo=r["tipo"])
            cu.subcuadros.append(s)
            self._seleccionar(s)

    def añadir_carga(self) -> None:
        cu = self._cuadro_actual()
        if cu is None:
            messagebox.showinfo(NOMBRE_APP, "Selecciona primero el cuadro donde añadir la carga.")
            return
        campos = [DCampo("tipo", "Tipo de carga", "combo", list(lc.TIPOS_CARGA), self._ultimo_tipo_carga),
                  DCampo("p", "P activa nominal [kW]", "float", "", True, minimo=0),
                  DCampo("n", "Cantidad", "float", "1", True, minimo=1, maximo=500, ayuda="nº de cargas iguales"),
                  DCampo("nombre", "Nombre")]
        r = Dialogo.pedir(self, f"Nueva carga en {cu.tag}", campos)
        if not r:
            return
        self._ultimo_tipo_carga = r["tipo"]
        ultima = None
        for _ in range(int(r["n"])):
            ultima = lc.Carga(r["tipo"], r["p"], nombre=r["nombre"])
            cu.cargas.append(ultima)
        self._seleccionar(ultima)

    def añadir_bateria(self) -> None:
        cu = self._cuadro_actual()
        if cu is None:
            messagebox.showinfo(NOMBRE_APP, "Selecciona primero el cuadro donde añadir la batería.")
            return
        campos = [DCampo("q", "Q de la batería [kVAr]", "float", "", True, minimo=0, excl=True),
                  DCampo("nombre", "Nombre"),
                  DCampo("fases", "Configuración de fases", "combo", list(lc.CONFIG_FASES), "3F")]
        r = Dialogo.pedir(self, f"Nueva batería en {cu.tag}", campos)
        if r:
            b = lc.BateriaCondensadores(r["q"], nombre=r["nombre"], config_fases=r["fases"])
            cu.baterias.append(b)
            self._seleccionar(b)

    def duplicar(self) -> None:
        sel = self._sel()
        if not sel:
            return
        o, tipo, padre = sel
        lista = self._lista_de(o, tipo, padre)
        n = 1
        if tipo == "carga":
            r = Dialogo.pedir(self, "Duplicar carga",
                              [DCampo("n", "Número de copias", "float", "1", True, minimo=1, maximo=500)])
            if not r:
                return
            n = int(r["n"])
        nuevo = None
        for _ in range(n):
            if tipo == "carga":
                nuevo = lc.Carga.from_dict(o.to_dict())
            elif tipo == "bat":
                nuevo = lc.BateriaCondensadores.from_dict(o.to_dict())
            else:
                nuevo = lc.Cuadro.from_dict(o.to_dict())
                usados = {c.tag for c in self.proy.todos_los_cuadros()}
                k, cand = 1, f"{o.tag}-copia"
                while cand in usados:
                    k += 1
                    cand = f"{o.tag}-copia{k}"
                nuevo.tag = cand
            lista.insert(lista.index(o) + 1, nuevo)
        self._seleccionar(nuevo)

    def reordenar(self, delta: int) -> None:
        sel = self._sel()
        if not sel:
            return
        o, tipo, padre = sel
        lista = self._lista_de(o, tipo, padre)
        i = lista.index(o)
        j = i + delta
        if 0 <= j < len(lista):
            lista[i], lista[j] = lista[j], lista[i]
            self._seleccionar(o)

    def mover(self) -> None:
        sel = self._sel()
        if not sel:
            return
        o, tipo, padre = sel
        destinos = {}
        if tipo == "cuadro":
            prohibidos = {id(x) for x in o.todos_los_cuadros()}
            destinos["(nivel superior)"] = None
        else:
            prohibidos = set()
        for cu in self.proy.todos_los_cuadros():
            if id(cu) not in prohibidos:
                destinos[f"{cu.tag} — {cu.nombre}"] = cu
        actual = next((k for k, v in destinos.items() if v is padre), None)
        if len(destinos) < 2 and actual is not None:
            messagebox.showinfo(NOMBRE_APP, "No hay otro cuadro al que mover este elemento.")
            return
        r = Dialogo.pedir(self, "Mover a…", [DCampo("dest", "Mover a", "combo", list(destinos), actual or "")])
        if not r or destinos.get(r["dest"], "x") is padre:
            return
        destino = destinos[r["dest"]]
        self._lista_de(o, tipo, padre).remove(o)
        if tipo == "carga":
            destino.cargas.append(o)
        elif tipo == "bat":
            destino.baterias.append(o)
        else:
            (destino.subcuadros if destino else self.proy.cuadros).append(o)
        self._seleccionar(o)

    def eliminar(self) -> None:
        sel = self._sel()
        if not sel:
            return
        o, tipo, padre = sel
        if tipo == "cuadro":
            todos = list(o.todos_los_cuadros())
            n_c = sum(len(x.cargas) for x in todos)
            msg = (f"¿Eliminar el cuadro {o.tag} con todo su contenido?\n"
                   f"({n_c} carga(s), {len(todos) - 1} subcuadro(s))")
        else:
            msg = f"¿Eliminar {o.tag}?"
        if not messagebox.askyesno(NOMBRE_APP, msg):
            return
        lista = self._lista_de(o, tipo, padre)
        i = lista.index(o)
        lista.remove(o)
        self.refrescar_ahora()
        vecinos = lista
        if vecinos:
            self.tree.selection_set(clave(vecinos[min(i, len(vecinos) - 1)]))
        elif padre is not None:
            self.tree.selection_set(clave(padre))
        self._mostrar_seleccion()

    # ═════════════════════════════════════════════════════════════════════════
    #  3. EQUIPOS DE EMERGENCIA
    # ═════════════════════════════════════════════════════════════════════════
    def _build_emergencia(self) -> None:
        pw = ttk.PanedWindow(self.tab_eme, orient="horizontal")
        pw.pack(fill="both", expand=True)
        izq = ttk.Frame(pw, padding=(0, 0, 6, 0))
        izq.columnconfigure(0, weight=1)
        izq.rowconfigure(1, weight=1)
        barra = ttk.Frame(izq)
        barra.grid(row=0, column=0, sticky="w", pady=(0, 4))
        ttk.Button(barra, text="+ Equipo", command=self.equipo_anadir).pack(side="left", padx=(0, 4))
        ttk.Button(barra, text="Eliminar", command=self.equipo_borrar).pack(side="left")
        marco, self.tree_eq = crear_tabla(
            izq, [("tipo", "TIPO", 80, "w"), ("se", "SEMER [kVA]", 105, "e"), ("sd", "S DISEÑO [kVA]", 120, "e")],
            altura=15, arbol=True, ancho_arbol=190, titulo_arbol="TAG · NOMBRE")
        marco.grid(row=1, column=0, sticky="nsew")
        self.tree_eq.bind("<<TreeviewSelect>>", lambda e: self._mostrar_equipo(forzar=False))
        self.tree_eq.bind("<Delete>", lambda e: self.equipo_borrar())
        pw.add(izq, weight=1)
        self.after(150, lambda: pw.sashpos(0, 520))

        der = ttk.Frame(pw)
        der.columnconfigure(0, weight=1)
        self.hint_eq = ttk.Label(der, text="Añade un grupo electrógeno o un SAI con «+ Equipo».",
                                 foreground="#666666", padding=20)
        self.hint_eq.grid(row=0, column=0, sticky="nw")
        self.form_eq = FormView(der, self._equipo_cambiado)
        self.form_eq.grid(row=0, column=0, sticky="new")
        self.form_eq.grid_remove()
        pw.add(der, weight=3)

    def _fila_equipo(self, e) -> tuple:
        return (f"{e.tag}  {e.nombre}".strip(), (e.tipo, t(e.s_emer_kva), t(e.s_diseno_kva, 1)))

    def _equipo_sel(self):
        s = self.tree_eq.selection()
        return next((e for e in self.proy.equipos if f"e{id(e)}" == s[0]), None) if s else None

    def _equipo_cambiado(self, reconstruir: bool) -> None:
        self.cambiado(reconstruir)

    def _mostrar_equipo(self, forzar: bool = True) -> None:
        e = self._equipo_sel()
        if not forzar and e is not None and e is self.form_eq.obj:
            return
        if e is None:
            self.form_eq.vaciar()
            self.form_eq.grid_remove()
            self.hint_eq.grid()
            return
        self.hint_eq.grid_remove()
        self.form_eq.grid()

        def set_tag(o, raw):
            raw = raw.strip()
            if not raw:
                raise ValueError("TAG: obligatorio")
            if any(x is not o and x.tag == raw for x in self.proy.equipos):
                raise ValueError("TAG: ya existe otro equipo con ese TAG")
            if raw != o.tag:        # renombrar también en las cargas y cuadros asociados
                for cu in self.proy.todos_los_cuadros():
                    if cu.tag_emer == o.tag:
                        cu.tag_emer = raw
                    for c in cu.cargas:
                        if c.tag_emer == o.tag:
                            c.tag_emer = raw
                o.tag = raw
        campos = [
            Campo("TAG (se aplica al salir del campo)", lambda o: o.tag, set_tag, commit=True),
            Campo("Nombre", *f_str("nombre"), ancho=30),
            Campo("Revisión", *f_str("rev"), ancho=10),
            Campo("Tipo de equipo", lambda o: o.tipo, lambda o, v: None, "texto", activo=lambda o: False),
            Campo("Forzar trifásico", lambda o: o.forzar_trifasico, lambda o, v: setattr(o, "forzar_trifasico", bool(v)),
                  "check"),
            Campo("Nº de horas requeridas", *f_num("horas", "Horas", 0, None, True), ancho=12),
        ]
        if e.tipo == "G. ELEC.":
            campos += [Campo("V depósito incluido [L]", *f_num("v_deposito_incl_l", "V depósito", 0), ancho=12),
                       Campo("Consumo [L/h]", *f_num("consumo_l_h", "Consumo", 0), ancho=12)]
            extra = [("V depósito adicional [L]", lambda o: t(o.v_deposito_ad_l, 0))]
        else:
            campos.append(Campo("Eficiencia [%]", *f_num("eficiencia_pct", "Eficiencia", 0, 100, True), ancho=12))
            extra = [("Capacidad de batería [kWh]", lambda o: t(o.cap_bateria_kwh))]
        res = [("Consumidores asociados", lambda o: str(o.n_consumidores)),
               ("SEMER [kVA]", lambda o: t(o.s_emer_kva)),
               ("Fases / tensión", lambda o: f"{o.fases} / {t(o.tension_v, 0)} V"),
               ("S ESP mín. [kVA]", lambda o: t(o.s_esp_min_kva)),
               ("S de diseño (tabla) [kVA]", lambda o: t(o.s_diseno_kva, 1))] + extra + [
               ("Avisos", lambda o: "; ".join(o.avisos) or NO_APLICA)]
        self.form_eq.cargar(e, campos, res, f"{e.tipo} {e.tag}")

    def equipo_anadir(self) -> None:
        def validar(r):
            if r["tag"] and any(x.tag == r["tag"] for x in self.proy.equipos):
                raise ValueError("Ya existe un equipo con ese TAG")
        campos = [DCampo("tipo", "Tipo de equipo", "combo", list(lc.TIPOS_EMERGENCIA), "G. ELEC."),
                  DCampo("tag", "TAG", ayuda="vacío = automático"),
                  DCampo("nombre", "Nombre")]
        r = Dialogo.pedir(self, "Nuevo equipo de emergencia", campos, validar)
        if not r:
            return
        e = lc.EquipoEmergencia(r["tipo"], tag=r["tag"] or self.proy.siguiente_tag_equipo(r["tipo"]),
                                nombre=r["nombre"] or lc.TIPOS_EMERGENCIA[r["tipo"]][1].capitalize())
        self.proy.equipos.append(e)
        self.refrescar_ahora()
        self.tree_eq.selection_set(f"e{id(e)}")
        self._mostrar_equipo()

    def equipo_borrar(self) -> None:
        e = self._equipo_sel()
        if e and messagebox.askyesno(NOMBRE_APP, f"¿Eliminar {e.tag}?\nSe desasociará de las cargas y cuadros que lo usan."):
            self.proy.desasociar_emergencia(e.tag)
            self.proy.equipos.remove(e)
            self.refrescar_ahora()
            self._mostrar_equipo()
            self._mostrar_seleccion()

    # ═════════════════════════════════════════════════════════════════════════
    #  4. RESUMEN
    # ═════════════════════════════════════════════════════════════════════════
    def _build_resumen(self) -> None:
        self.nb_res = ttk.Notebook(self.tab_res)
        self.nb_res.pack(fill="both", expand=True)

        def pagina(titulo, columnas, **kw):
            f = ttk.Frame(self.nb_res, padding=4)
            f.columnconfigure(0, weight=1)
            f.rowconfigure(0, weight=1)
            marco, tree = crear_tabla(f, columnas, altura=18, **kw)
            marco.grid(row=0, column=0, sticky="nsew")
            self.nb_res.add(f, text=titulo)
            return f, tree

        _, self.r_cuadros = pagina("Cuadros", [
            ("nombre", "NOMBRE", 220, "w"), ("tipo", "TIPO", 105, "w"), ("zona", "ZONA", 55, "w"),
            ("trafo", "TRAFO", 60, "w"), ("pi", "P inst. [kW]", 100, "e"), ("p", "P [kW]", 80, "e"),
            ("q", "Q [kVAr]", 80, "e"), ("s", "S [kVA]", 80, "e"), ("cos", "cos φ", 55, "e"),
            ("ks", "Ks global", 80, "e"), ("se", "S EMER [kVA]", 105, "e")],
            arbol=True, ancho_arbol=150, titulo_arbol="TAG")
        _, self.r_cargas = pagina("Todas las cargas", [
            ("cuadro", "CUADRO", 70, "w"), ("nombre", "NOMBRE", 140, "w"), ("tipo", "TIPO", 90, "w"),
            ("fases", "CONF. FASES", 85, "center"), ("u", "TENSIÓN NOM. [V]", 110, "e"),
            ("p", "P ACTIVA NOM. [kW]", 120, "e"), ("cos", "cos φ", 55, "e"), ("ku", "Ku", 45, "e"),
            ("ks", "Ks", 45, "e"), ("pd", "P [kW]", 75, "e"), ("qd", "Q [kVAr]", 75, "e"), ("sd", "S [kVA]", 75, "e")],
            arbol=True, ancho_arbol=100, titulo_arbol="TAG")
        _, self.r_trafos = pagina("Transformadores", [
            ("p", "P [kW]", 90, "e"), ("q", "Q [kVAr]", 90, "e"), ("s", "S [kVA]", 90, "e"),
            ("adm", "S ADM. [kVA]", 100, "e"), ("oc", "OCUPACIÓN", 90, "e")],
            arbol=True, ancho_arbol=100, titulo_arbol="TRAFO")
        _, self.r_equipos = pagina("Equipos de emergencia", [
            ("nombre", "NOMBRE", 170, "w"), ("tipo", "TIPO", 80, "w"), ("n", "Nº CONS.", 65, "e"),
            ("fases", "FASES", 55, "center"), ("u", "U [V]", 50, "e"), ("se", "SEMER [kVA]", 90, "e"),
            ("sesp", "S ESP mín [kVA]", 105, "e"), ("sd", "S DISEÑO [kVA]", 100, "e"), ("h", "HORAS", 55, "e"),
            ("dep", "V DEP. AD. [L]", 95, "e"), ("ef", "EFIC. [%]", 70, "e"), ("bat", "CAP. BAT. [kWh]", 105, "e")],
            arbol=True, ancho_arbol=70, titulo_arbol="TAG")
        self.tab_avisos = ttk.Frame(self.nb_res, padding=4)
        self.tab_avisos.columnconfigure(0, weight=1)
        self.tab_avisos.rowconfigure(0, weight=1)
        self.lst_avisos = tk.Listbox(self.tab_avisos, activestyle="none")
        sb = ttk.Scrollbar(self.tab_avisos, orient="vertical", command=self.lst_avisos.yview)
        self.lst_avisos.configure(yscrollcommand=sb.set)
        self.lst_avisos.grid(row=0, column=0, sticky="nsew")
        sb.grid(row=0, column=1, sticky="ns")
        self.nb_res.add(self.tab_avisos, text="Avisos")

    def _refrescar_resumen(self) -> None:
        p = self.proy
        for tr in (self.r_cuadros, self.r_cargas, self.r_trafos, self.r_equipos):
            tr.delete(*tr.get_children(""))

        def cuadro(cu, padre_iid):
            iid = self.r_cuadros.insert(padre_iid, "end", text=cu.tag, open=True, values=(
                cu.nombre, cu.tipo, cu.zona_efectiva or NO_APLICA, cu.trafo_efectivo or NO_APLICA, t(cu.p_inst_kw),
                t(cu.p_kw), t(cu.q_kvar), t(cu.s_kva), t(cu.cos_phi, 3), t(cu.ks_global, 3), t(cu.s_emer_kva)))
            for s in cu.subcuadros:
                cuadro(s, iid)
        for c in p.cuadros:
            cuadro(c, "")
        for cu in p.todos_los_cuadros():
            for c in cu.cargas:
                self.r_cargas.insert("", "end", text=c.tag, values=(
                    cu.tag, c.nombre, c.tipo, c.fases, t(c.tension_v, 0), t(c.p_nom_kw), t(c.cos_efectivo, 3),
                    t(c.ku), t(c.ks), t(c.p_kw), t(c.q_kvar), t(c.s_kva)))
        for tag, (pp, q, s, adm) in p.ocupacion_trafos.items():
            self.r_trafos.insert("", "end", text=tag, values=(
                t(pp), t(q), t(s), t(adm), f"{t(s / adm * 100, 1)} %" if adm else NO_APLICA))
        for e in p.equipos:
            self.r_equipos.insert("", "end", text=e.tag, values=(
                e.nombre, e.tipo, e.n_consumidores, e.fases, t(e.tension_v, 0), t(e.s_emer_kva), t(e.s_esp_min_kva),
                t(e.s_diseno_kva, 1), t(e.horas, 1), t(e.v_deposito_ad_l, 0),
                t(e.eficiencia_pct, 1) if e.tipo == "UPS" else NO_APLICA, t(e.cap_bateria_kwh)))
        self.lst_avisos.delete(0, "end")
        for a in p.avisos:
            self.lst_avisos.insert("end", "⚠ " + a)
        if not p.avisos:
            self.lst_avisos.insert("end", "Sin avisos.")


# ═════════════════════════════════════════════════════════════════════════════
#  MAIN
# ═════════════════════════════════════════════════════════════════════════════

def main(argv: Optional[list] = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if sys.platform.startswith("win"):
        try:    # pantallas de alta densidad: nitidez en Windows
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    ruta = argv[0] if argv and not argv[0].startswith("-") else None
    try:
        app = AppGUI(ruta)
    except tk.TclError as e:
        print(f"No se pudo abrir la ventana gráfica: {e}\n"
              "¿Hay un entorno gráfico disponible? (en un servidor sin pantalla usa lista_cargas.py)",
              file=sys.stderr)
        return 1
    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())