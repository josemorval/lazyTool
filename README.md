# <img src="assets/brand/lazytool_icon.png" width="100" alt="lazyTool icon"> lazyTool

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Windows](https://img.shields.io/badge/platform-Windows-0078D4)
![DirectX 11](https://img.shields.io/badge/graphics-DirectX%2011-lightgrey)
![Dear ImGui](https://img.shields.io/badge/UI-Dear%20ImGui-ff69b4)

**lazyTool** es un editor experimental para crear escenas graficas en DirectX 11. Esta pensado para personas que quieren construir una imagen paso a paso, ver que recursos usa cada paso, ajustar parametros en directo y exportar el resultado como ejecutable.

No intenta esconder la parte tecnica. La idea es que puedas abrir la herramienta, mirar la lista de recursos, mirar la lista de comandos, seleccionar algo y entender que esta pasando.

![lazyTool editor screenshot](docs/images/editor-main.png)

## Que puedes hacer

- Ver una escena en tiempo real dentro del viewport.
- Crear recursos visuales y buffers internos.
- Encadenar comandos de render en una lista ordenada.
- Limpiar targets, dibujar, despachar compute y organizar comandos en grupos.
- Editar parametros desde paneles sin recompilar toda la aplicacion.
- Mover la camara, activar rejilla, encuadrar objetos y usar controles de transformacion.
- Ver avisos, errores y mensajes en el log.
- Revisar relaciones entre comandos y recursos en una vista de grafo.
- Animar valores, camara, luces y estados con una timeline.
- Exportar una escena como ejecutable standalone.
- Generar una version 64k/procedural mas estricta cuando el contenido lo permite.

## Para quien es

lazyTool puede ser util si estas aprendiendo rendering, si quieres prototipar efectos visuales, si te gustan las demos procedurales o si prefieres una herramienta donde el pipeline sea visible.

No necesitas conocer todo el motor para empezar. Lo normal es abrir una escena sencilla, seleccionar comandos y recursos, cambiar valores y observar que ocurre en el viewport.

## Como se organiza la interfaz

| Zona | Para que sirve |
|---|---|
| **Viewport** | Muestra la imagen final y los controles visuales de camara, rejilla y transformacion. |
| **Resources** | Contiene texturas, render targets, buffers, meshes, valores y recursos internos. |
| **Command Pipeline** | Es la lista de pasos que se ejecutan cada frame. |
| **Inspector** | Cambia las opciones del recurso o comando seleccionado. |
| **User CB** | Edita valores que se envian a la GPU como parametros. |
| **Render Graph** | Ayuda a entender que comando lee o escribe cada recurso. |
| **Timeline** | Permite animar valores y estados con claves en el tiempo. |
| **Log** | Muestra errores, advertencias y mensajes importantes. |

## Flujo de trabajo basico

1. Abre lazyTool.
2. Mira el viewport para ver el resultado actual.
3. Selecciona un recurso o comando.
4. Cambia valores desde el Inspector.
5. Usa el log si algo no se ve como esperas.
6. Guarda el archivo `.lt`.
7. Exporta un ejecutable cuando quieras compartir la escena.

El archivo `.lt` es texto plano. Eso facilita revisar cambios, versionar escenas y entender que ha guardado el editor.

## Recursos

Los recursos son las piezas que usa la escena. Pueden ser valores simples, texturas, targets internos, buffers, meshes o recursos incorporados como color de escena, profundidad, tiempo o mapa de sombras.

La ventaja de tenerlos visibles es que puedes saber rapidamente que existe en la escena y que esta usando cada comando.

## Comandos

Los comandos son los pasos del frame. Se ejecutan en orden y forman el pipeline principal.

Algunos comandos limpian una textura, otros dibujan, otros ejecutan trabajo de compute y otros agrupan o repiten pasos. Esta estructura hace que el frame sea facil de inspeccionar: si algo sale mal, puedes buscar el paso exacto que produce el problema.

## Timeline

La timeline permite animar valores sin escribir codigo nuevo para cada cambio. Puedes usarla para variar parametros, activar o desactivar comandos, mover transformaciones, cambiar la camara o ajustar la luz direccional.

Esta pensada para iterar rapido: pones claves, reproduces la escena y ajustas.

## Render Graph

El Render Graph no es el editor principal de la escena. Es una vista de ayuda.

Sirve para ver dependencias: que comando escribe un recurso, que comando lo lee despues, donde aparece una textura intermedia o por que un paso puede estar usando algo inesperado.

## Exportacion

lazyTool tiene dos caminos de salida:

| Salida | Uso |
|---|---|
| **Export normal** | Crea un ejecutable standalone con los datos necesarios. |
| **build64k** | Genera un player C compacto para escenas procedurales y elimina codigo de features que no se usan. |

El camino 64k es mas limitado a proposito. Esta pensado para reducir tamano y mantener solo lo necesario: si una escena no usa timeline, compute, buffers o draw, esas partes no tienen por que entrar en el player generado.

## Compilar

Necesitas Windows, Visual Studio con MSVC, Windows SDK y una GPU compatible con DirectX 11.

Abre una **Developer Command Prompt for Visual Studio** en la carpeta del repositorio y ejecuta:

```bat
build.bat
```

Tambien puedes elegir perfil:

```bat
build.bat fast
build.bat profile
build.bat release
```

El build genera los ejecutables en `bin/` y lanza el editor.

## Atajos utiles

| Atajo | Accion |
|---|---|
| `F5` | Recompilar todo lo necesario para refrescar la escena. |
| `Ctrl+S` | Abrir guardado del archivo actual. |
| `Space` | Pausar o reproducir la escena. |
| `F6` | Reiniciar el tiempo de escena. |
| `F11` | Alternar fullscreen del viewport. |
| `Delete` | Borrar el elemento seleccionado. |

## Estado actual

lazyTool es una herramienta experimental. Algunas partes son muy directas y tecnicas porque el objetivo es aprender, iterar y mantener el pipeline visible.

Si estas empezando, lo mejor es tocar pocos parametros cada vez y mirar el resultado. El editor esta pensado para que puedas explorar sin tener que entender todo desde el primer dia.

## Carpetas importantes

| Carpeta | Contenido |
|---|---|
| `src/` | Codigo del editor, runtime y sistemas principales. |
| `assets/` | Recursos usados por la aplicacion. |
| `docs/` | Capturas y documentacion auxiliar. |
| `build64k/` | Generador y player compacto para export 64k/procedural. |
| `external/` | Dependencias de terceros incluidas en el repositorio. |

## Licencias y dependencias

El codigo usa varias librerias conocidas del ecosistema C/C++ grafico, incluyendo Dear ImGui, stb, cgltf y NanoSVG. Los iconos de la interfaz usan Lucide; su licencia esta en `assets/icons/LUCIDE-LICENSE.txt`.
