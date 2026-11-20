# Electrical Tools

Repositorio: https://github.com/APES-Power/electrical-tools · Identificador: `com.apespower.electricaltools`

Aplicación nativa para Linux que conserva el formato `.list` JSON de la versión Python (archivada, junto con el
prototipo en C, en `obs/`).

## Dependencias (Debian/Ubuntu)

```sh
sudo apt install build-essential cmake pkg-config libgtk-4-dev libadwaita-1-dev nlohmann-json3-dev
```

## Compilar y ejecutar

```sh
cmake -S . -B build
cmake --build build -j
./build/electrical-tools
./build/electrical-tools ejemplo.list
```

Para instalarla con su icono y lanzador (menú de aplicaciones / dock):

```sh
cmake --install build --prefix ~/.local
```

Los iconos de la aplicación están en `res/iconos` (SVG simbólicos: los de Adwaita que usa la aplicación,
`plug-symbolic` para las cargas, `uninterruptible-power-supply-symbolic` para el SAI y `charge-symbolic` para la
batería de condensadores; `plug` y `charge` son del kit de iconos de Icon Library, CC0). Se incrustan en el ejecutable al compilar (cabecera generada `iconos.h`) y al
arrancar se instalan como un tema de iconos propio («ElectricalTools», en `~/.cache/electrical-tools/temas`) que la
aplicación activa y que hereda del tema del escritorio: así se ven igual en cualquier escritorio. Para
añadir o cambiar un icono basta con dejar el `.svg` en `res/iconos` y volver a compilar.

El logo es `res/logo.svg`: se incrusta en el ejecutable al compilar (cabecera generada `logo_svg.h`), se usa
como icono de las ventanas y aparece en «Acerca de» (botón ⓘ de la barra), que enlaza también al repositorio de GitHub. `cmake --install` copia además
el icono al tema `hicolor` y el lanzador `res/com.apespower.electricaltools.desktop`.

El programa permite crear/abrir/guardar proyectos, gestionar cuadros, cargas y fuentes, y exportar con el
botón «Exportar» de la barra (único): lista de cargas en Excel y arquitectura eléctrica en SVG, DXF o DWG.
**Ctrl+Z** deshace el último cambio del proyecto (arquitectura incluida: mover, redimensionar, conectar,
crear o borrar bloques, trazado de flechas...) y **Ctrl+Mayús+Z** / **Ctrl+Y** lo rehace. Los archivos `.list` existentes son compatibles.

### Datos generales

Zonas, transformadores, grupos electrógenos, instalaciones fotovoltaicas y prefijos de TAG, cada uno con
su lista y botones de añadir/editar/eliminar:

- **Zonas**: TAG (letras y números) y descripción. Se asignan a los cuadros; renombrar una zona actualiza
  los cuadros que la usan.
- **TAG automáticos**: `<zona>-<TAG del tipo>-<nº de 2 dígitos>`, p. ej. `PS-M-01` (sin zona: `M-01`). La
  zona es la de la carga si se le asigna una en su diálogo; si no, la de su cuadro. La numeración es
  correlativa por zona y tipo.
- **Tipos de carga** (botón «Editar…» de la pestaña, que abre una ventana con la tabla): TAG, descripción, cos φ (fijo o «tabla» de motores) y
  rendimiento («No aplica», «P absorbida = P/η» o «Informativo»), con el nº de cargas de cada tipo. Las
  celdas se editan con un clic (Intro para aplicar). En las cargas el tipo se elige por su descripción, que
  es obligatoria y única (sin distinguir mayúsculas); es también lo que se muestra en el árbol y en el
  Excel. Internamente cada tipo conserva un identificador fijo, así que cambiar la descripción o el TAG no
  rompe las cargas existentes. Un tipo con cargas no se puede eliminar; se pueden añadir tipos nuevos y restablecer los de serie. La última
  fila es la batería de condensadores (sólo su TAG, `CAP` de serie). Se guardan en
  `datos_generales.tipos_carga` y `prefijo_bateria` (los `prefijos_tag` de ficheros anteriores se respetan).
- **Transformadores**: datos de una unidad (TAG, potencia nominal, tensión secundaria y potencia admisible)
  y, aparte, el número de transformadores iguales en paralelo; los cálculos usan la potencia total. Muestra los CGBT que alimenta, la S
  demandada y el índice de carga (aviso si supera la S admisible). Renombrar un trafo actualiza los cuadros.
- **Grupos electrógenos**: TAG, nombre, autonomía, consumo, depósito incluido y forzar trifásico. La potencia
  sale de las cargas/cuadros asociados (Ke); se muestra el grupo normalizado y el depósito adicional.
- **SAI**: TAG, nombre, autonomía, rendimiento, forzar trifásico y cuadro de alimentación. La potencia sale de las cargas/cuadros
  asociados (Ke); se muestran el SAI normalizado y la capacidad de las baterías.
- **Fotovoltaica**: TAG, nombre, kWp, potencia y número de inversores, fases y cuadro de conexión. Calcula la
  potencia AC y el ratio DC/AC, con avisos (ratio fuera de 0,8–1,3, cuadro inexistente, P AC > S admisible
  del trafo...). Se guarda en `datos_generales.fotovoltaica`.

Los datos de portada (empresa, código de documento...) se gestionan fuera de la aplicación: se conservan en
el `.list` pero no se muestran ni se exportan.

### Arquitectura eléctrica

Lienzo al estilo draw.io con los cuadros y sus fuentes. Bloques: cuadros (CGBT de origen en amarillo,
subcuadros en azul), transformadores (rojo), grupos electrógenos (verde), SAI (morado) e instalaciones
fotovoltaicas (naranja). Flechas: cuadro → subcuadro (negra), trafo → CGBT (roja), grupo electrógeno →
cuadro con cargas o cuadro asociado en emergencia (verde discontinua), SAI → cuadro (morada
discontinua), cuadro → SAI que alimenta (negra) y fotovoltaica → cuadro de conexión (naranja
discontinua). El SAI se representa con un símbolo de batería.

- Un cuadro puede alimentar a un SAI: arrastrar la flecha azul del cuadro hasta el SAI (o elegir el
  «Cuadro de alimentación» en su ficha). «Organizar» coloca el SAI debajo del cuadro que lo alimenta.
- Un cuadro puede colgar de un SAI: arrastrar la flecha azul del SAI hasta el cuadro (o hacer clic en ella
  para crear uno nuevo). Flecha morada continua SAI → cuadro. El cuadro queda asociado al SAI en
  emergencia (cuenta para dimensionarlo) y, en la jerarquía, pasa a ser subcuadro del cuadro que alimenta
  al SAI, así que su potencia llega aguas arriba (CGBT → SAI → cuadro); si el SAI no tiene cuadro de
  alimentación, queda como origen. Si el SAI cambia de cuadro de alimentación, sus cuadros le siguen.
  Supr sobre la flecha lo suelta (pasa a ser un origen). Se guarda en `sai_alimentacion` del cuadro.

- Arrastrar la flecha azul de una fuente hasta un cuadro la conecta: un trafo sólo a un CGBT, un grupo
  asocia el cuadro en emergencia y una FV fija su cuadro de conexión. Un clic en la flecha azul de un
  trafo crea un CGBT alimentado por él.
- Supr sobre una flecha de fuente la desconecta; sobre una fuente, la elimina (con confirmación). Doble
  clic en una fuente abre su ficha. «Equipo» en la barra añade trafos, grupos, SAI, FV y baterías de condensadores.

- **Doble clic** en el lienzo vacío: nuevo cuadro general (origen). Doble clic en un bloque (o Intro): editar.
- Al pasar el ratón sobre un bloque aparecen **flechas azules**: clic para crear un subcuadro en esa
  dirección; arrastrar hasta otro bloque para conectarlo (pasa a colgar de él); arrastrar al vacío
  para crear un subcuadro ahí. No se permiten conexiones en ciclo.
- **Edición de flechas (como en draw.io)**: al seleccionar una flecha aparecen sus asas.
  - Extremos (círculos): arrastrarlos a otro bloque reconecta la flecha (p. ej. un subcuadro pasa a
    colgar de otro cuadro, o un CGBT a otro trafo); soltarlos sobre el mismo bloque cambia el lado por
    el que sale o entra. Las conexiones no válidas se rechazan con aviso.
  - Tramo intermedio (rombo): arrastrarlo desplaza el tramo.
  - «Trazado automático» devuelve la flecha a su recorrido por defecto. Los trazados se guardan en el
    `.list` (clave `diagrama_flechas`, por TAG de los bloques).
- **Redimensionar**: al seleccionar un bloque aparecen 8 asas (esquinas y centros de los lados); el texto se
  adapta a la altura (mínimo 100 × 40). El tamaño se guarda en la clave `diagrama` (`w`, `h`).
- **Exportar** (botón «Exportar» de la barra superior): SVG (igual que en pantalla, sin rejilla ni selección), DXF R12 (capas
  por tipo de bloque y de flecha, flechas de emergencia, SAI y FV con línea discontinua; textos con el
  estilo ARIAL, fuente `arial.ttf`) y DWG. El DWG se
  obtiene convirtiendo el DXF con *ODA File Converter* o con `dxf2dwg` de LibreDWG si alguno está en el
  PATH; si no hay conversor, se guarda el DXF (que AutoCAD abre directamente) y se avisa.
- **Ajuste al centro (como draw.io)**: al mover un bloque, su centro se alinea con el de los bloques
  cercanos (primero los conectados) y se muestran guías azules; así las flechas quedan rectas. Al
  arrastrar una flecha sobre un bloque se muestran sus nodos de conexión: tres por lado (25 %, 50 % y
  75 %) y el centro del bloque. Soltar en un nodo fija la flecha a ese punto; soltar en el centro deja la
  conexión automática. Al pasar el ratón por un bloque también se ven sus nodos (aspas «×»), y
  arrastrando desde uno se crea una flecha que sale de ese punto. Las
  flechas llegan siempre al centro del lado.
- Arrastrar un bloque lo mueve (con rejilla); arrastrar el fondo desplaza la vista; Ctrl + rueda: zoom.
- **Supr** elimina el cuadro seleccionado (con confirmación) o, si hay una flecha seleccionada, la
  desconecta. «Organizar» coloca los bloques en árbol automáticamente.
- Los datos del cuadro (doble clic) incluyen la lista de sus cargas (clic en una para editarla) y un botón
  para añadir cargas.
- **Equipos** (botón «Equipo» de la barra): transformador, grupo electrógeno, SAI, fotovoltaica y
  **batería de condensadores**. La batería (bloque azul con símbolo de condensador) sólo puede ir en un
  CGBT, uno por CGBT; su potencia se calcula automáticamente con la P y la Q demandadas totales del CGBT
  (subcuadros incluidos) para alcanzar el cos φ objetivo, redondeando al escalón de regulación
  (Q = P·(tan φ₁ − tan φ₂)). Doble clic para cambiar cos φ objetivo y escalón; Supr para quitarla;
  arrastrar el extremo de su flecha a otro CGBT la traslada.
- La posición de cada bloque se guarda en el `.list` (clave opcional `diagrama` de cada cuadro, trafo,
  equipo de emergencia y FV).

### Cuadros y cargas

Árbol CGBT → subcuadros → cargas/baterías con la revisión de cada carga (columna «Rev.», que se rellena
directamente en la lista con un clic; es el único sitio donde se gestionan revisiones), P instalada, factores Ku, Ks y Ke (en los cuadros, Ks es la
simultaneidad global), P/Q/S demandadas, cos φ, equipo de emergencia, longitud y avisos de cada nodo. Dentro
de cada cuadro, el desplegable «Equipos» agrupa su batería de condensadores y los equipos de emergencia
conectados a él (el SAI, en el cuadro que lo alimenta; el grupo electrógeno, en los cuadros a los que da
servicio); los equipos sin conectar van en «SIN CONECTAR». Las columnas tienen ancho fijo y se pueden redimensionar (desplazamiento horizontal). Doble clic en una carga para editarla o eliminarla.

- **Mover cargas…**: selecciona una o varias cargas (Ctrl o Mayús + clic) y elige el cuadro de destino.
- **Añadir carga…** (o el botón `+` de la barra, o doble clic sobre un cuadro/carga) abre un diálogo que pide
  cuadro de alimentación, tipo, nombre, TAG, revisión, cantidad, P nominal, fases, cos φ, rendimiento,
  Ku/Ks/Ke, equipo de emergencia, zona (opcional) y longitud de la línea hasta el cuadro, y muestra el resultado del cálculo mientras se rellenan los datos.
- **Exportar a Excel…** genera un `.xlsx` (sin dependencias externas) con las hojas *Lista de cargas*,
  *Cuadros*, *Emergencia* y *Avisos*, con cabeceras fijadas y autofiltro.
