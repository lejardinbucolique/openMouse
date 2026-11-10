# openMouse: guía de uso

*[English version](HELP.md) · [Version française](AIDE.md) · [Deutsche Version](HILFE.md) · [Русская версия](SPRAVKA.md)*

> Esta traducción se hizo con la ayuda de una IA y no la ha revisado ninguna persona que
> hable español. Las correcciones son bienvenidas: infórmelas como incidencia (issue) en el
> [repositorio de GitHub](https://github.com/lejardinbucolique/openMouse/issues).

openMouse da a cada ratón conectado su propio puntero en el mismo escritorio de Windows.
Dos personas pueden así trabajar a la vez en un solo PC, cada una con su ratón: una hace
clic aquí mientras la otra desplaza una página allá.

## Antes de empezar

- openMouse sustituye la flecha de Windows por un puntero de color por cada ratón. De forma
  predeterminada, el primero es amarillo y el segundo naranja.
- Solo una persona escribe a la vez: Windows envía las pulsaciones únicamente a la ventana
  activa. Los ratones son independientes, los teclados no.
- Necesita Windows 10 u 11 de 64 bits y al menos dos ratones (el panel táctil de un portátil
  cuenta como uno).

## 1. Inicio

1. Ejecute `openmouse.exe`. No hay instalador.
2. Windows pide permiso de administrador: acéptelo. Sin él, openMouse funciona igualmente,
   pero las ventanas iniciadas «como administrador» (el Administrador de tareas, por
   ejemplo) escapan al control de los ratones.
3. Aparece un icono junto al reloj y en la pantalla hay dos punteros de distinto color.

Para que se inicie con Windows: haga clic en el icono, **Configuración…**, pestaña
**General**, marque **Iniciar openMouse con Windows**. Esto solo está disponible cuando
openMouse está instalado en una carpeta protegida como `Archivos de programa`, porque
entonces el programa se inicia con todos los derechos; desde otra carpeta (Descargas,
Escritorio…) la casilla aparece atenuada y lo indica.

## 2. Conectar dos ratones

Basta con conectar el segundo ratón: aparece un nuevo puntero de otro color. Al
desconectarlo, su puntero desaparece; al volver a conectarlo, reaparece donde se quedó. El
último puntero nunca se oculta, así que nunca se queda sin ninguno.

Cada ratón conserva su color, incluso en otro puerto USB.

Si dos punteros se intercambian o un ratón maneja el puntero equivocado (puede ocurrir con
los receptores inalámbricos), desconecte y vuelva a conectar el receptor de ese ratón y
abra después **Configuración…** para ver los ratones reconocidos.

## 3. Usar los dos ratones

- **Mover**: cada ratón mueve su propio puntero sin molestar al otro.
- **Hacer clic**: el clic cae donde apunta el ratón que lo hace. Hacer clic en una ventana
  que no está activa la activa y hace clic en una sola pulsación.
- **Desplazar**: la rueda desplaza la ventana que hay bajo su propio puntero, sin necesidad
  de hacer clic.
- **Arrastrar** (mover un archivo, seleccionar): mientras una persona mantiene pulsado un
  botón, los clics de la otra esperan a que termine el gesto. Los punteros siguen libres.
- Los **botones Atrás y Adelante** del ratón también funcionan, cada uno para su ratón.

## 4. Configuración

Haga clic en el icono junto al reloj y luego en **Configuración…**. La ventana tiene dos
pestañas (**Ctrl + Tab** cambia de una a otra). Cada cambio se aplica y se guarda al
instante; **Deshacer mis cambios** restablece los valores de los ratones con los que se
abrió la ventana. Estos dos botones y la línea de estado («Aplicado y guardado») están
abajo, compartidos por las dos pestañas.

### Pestaña General

- **Iniciar openMouse con Windows**: vea «Inicio» más arriba.
- **Idioma**: «Automático (idioma de Windows)», «Français», «English», «Deutsch» o
  «Español». La elección se aplica al instante a la ventana, al menú del icono y a los
  mensajes de openMouse, y se recuerda.
- **Ayuda**: abre esta guía, en el idioma elegido, en GitHub.
- **Acerca de**: los créditos, con el enlace al sitio de Calepitool.

### Pestaña Ratones

Una zona por cada ratón y otra para el panel táctil de un portátil. Al pasar el puntero
sobre **Sensibilidad fija**, **Aceleración progresiva** o **Aprender un botón** aparece una
breve información emergente.

### Reconocer y apartar un ratón

El título de cada zona indica la marca del ratón («Ratón Logitech»), leída del propio ratón
o, si no es posible, de su fabricante. Cuando no se puede leer la marca, el título sigue
siendo «Ratón» seguido de un número; dos ratones de la misma marca también llevan su número.

La casilla **Desactivar**, a la derecha del título, aparta el ratón sin desconectarlo: su
puntero desaparece y nada de lo que hace se tiene en cuenta, al instante. Al desmarcarla,
vuelve donde se quedó, con su color y su velocidad. El último ratón que funciona no se puede
desactivar (el panel táctil cuenta mientras se usa): openMouse lo rechaza, así que nunca se
queda sin puntero.

### Aspecto del puntero

- **Color**: diez colores, para reconocer su puntero de un vistazo.
- **Estilo del puntero**: clásico, borde claro (el más visible, el predeterminado), cuerpo
  blanco con borde de color, grande, o resplandor suave. La vista previa muestra el
  resultado sobre cuatro fondos.

### Velocidad del puntero

Cada ratón sigue uno de dos modos (los controles del otro modo aparecen atenuados):

- **Sensibilidad fija**: cada movimiento se multiplica por el mismo valor. Es lo más
  sencillo: suba el valor si el puntero va demasiado lento.
- **Aceleración progresiva**: lento en los gestos pequeños (precisión), rápido en los
  amplios (cruzar la pantalla). El **umbral de aceleración** fija a partir de dónde acelera.

**Valores predeterminados** restablece la configuración original.

### Mantener dentro de la pantalla

Para cada ratón puede limitar el puntero a una pantalla (útil cuando cada persona trabaja en
su propio monitor) o dejarlo moverse por todo el escritorio.

### Aprender un botón

A veces un ratón tiene botones que Windows no conoce. Haga clic en **Aprender un botón** y
pulse el botón en un plazo de ocho segundos. Después elija qué hace: un clic (izquierdo,
derecho, central, Atrás, Adelante), una combinación de teclas, o nada. El clic se produce en
la posición del puntero de ese mismo ratón. **Olvidar** elimina el botón seleccionado.

Algunos botones no se pueden aprender: los que solo lee el software del fabricante no envían
nada a Windows. La ventana se lo indica.

## 5. Cuando algo va mal

| Problema | Qué hacer |
|---|---|
| No hay puntero, o el puntero sigue oculto | Pulse **Ctrl + Alt + Mayús + Q**: openMouse se detiene y devuelve el puntero de Windows. Si no, ejecute `openmouse.exe --restore-cursor`. |
| Tras Ctrl + Alt + Supr o al bloquear la sesión | Es normal: Windows toma el control. Al volver, openMouse se reanuda solo. |
| Una ventana de administrador ignora los ratones | Reinicie openMouse y acepte el permiso de administrador. |
| Un puntero sigue al ratón equivocado | Vuelva a conectar el ratón y compruebe después los ratones reconocidos en **Configuración…**. |
| Cualquier otra cosa | **Abrir la carpeta del registro** en el menú del icono: `openmouse.log` dice lo que ha pasado. |

## 6. Límites conocidos

- **Un solo teclado activo a la vez.** Dos personas no pueden escribir a la vez en dos
  ventanas distintas.
- **Barra de tareas, menú Inicio y notificaciones.** Allí solo es visible el puntero del
  ratón que «tiene» en ese momento el puntero real de Windows. El otro puntero pasa por
  detrás: para usarlos, haga primero clic con ese ratón en una ventana normal, o deje que lo
  maneje la otra persona.
- **Un arrastre que empieza directamente en una ventana inactiva** se pierde: haga clic una
  vez para activar la ventana y arrastre después.
- **Desplazamiento simultáneo**: si las dos personas desplazan en el mismo instante, un paso
  de la rueda puede ir a veces a la otra ventana.
- El sonido, el portapapeles y el orden de las ventanas siguen siendo compartidos por las
  dos personas.

Para los detalles técnicos (compilación, opciones de línea de comandos, archivo de
configuración), consulte el [README](../README.md).
