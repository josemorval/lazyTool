# lazyTool

**lazyTool** es un editor experimental de render graph 3D hecho en C++17, Win32, DirectX 11 e ImGui. Está pensado como una herramienta rápida para montar escenas, probar shaders HLSL, depurar recursos GPU y experimentar con pipelines de render en tiempo real sin tener que recompilar el motor por cada cambio visual.

> Este README se centra en lo que el editor puede hacer ahora mismo. La parte de gestión/catálogo de proyectos queda fuera por ahora.

---

## Qué es

lazyTool es un entorno de edición y ejecución donde combinas:

- **Recursos GPU**: texturas, render targets, buffers, meshes, shaders y valores.
- **Commands**: operaciones ordenadas que limpian targets, dibujan meshes, lanzan compute shaders o agrupan pasos.
- **Viewport en tiempo real**: una superficie de escena que se renderiza cada frame y se puede pausar, reiniciar o ver a pantalla completa.
- **Inspector editable**: panel para modificar recursos, estados de render, bindings, parámetros de shader, transforms y configuración de cámara/luz.
- **HLSL dinámico**: shaders VS/PS y CS compilados en runtime, con fallback si algo falla.

La idea principal es construir pipelines de render paso a paso: primero declaras o creas recursos, luego añades commands que los leen/escriben, y finalmente inspeccionas el resultado directamente en la UI.

---

## Stack técnico

- **Lenguaje:** C++17
- **Plataforma:** Windows
- **API gráfica:** DirectX 11
- **UI:** Dear ImGui
- **Shaders:** HLSL, Shader Model 5.0
- **Carga de imágenes:** stb_image
- **Carga de meshes:** cgltf
- **Build:** `build.bat` usando MSVC / Visual Studio Developer Command Prompt

---

## Requisitos

Para compilar y ejecutar desde código fuente necesitas:

- Windows.
- GPU compatible con DirectX 11.
- Visual Studio o Build Tools con `cl.exe`, `rc.exe` y librerías de Windows/DX11 disponibles.
- Ejecutar el build desde una **Developer Command Prompt for Visual Studio** o una consola equivalente con MSVC en el `PATH`.

---

## Build y ejecución

Desde la raíz del repo:

```bat
build.bat
```

Build debug:

```bat
build.bat debug
```

Build release y ejecución automática:

```bat
build.bat run
```

Build debug y ejecución automática:

```bat
build.bat debug run
```

El ejecutable se genera en:

```text
bin/lazyTool.exe
```

Durante el build también se copian los assets y shaders necesarios al directorio `bin/`.

---

## Vista general de la interfaz

La UI está organizada como un workspace de varias columnas:

| Zona | Qué permite hacer |
|---|---|
| **Top bar** | Crear una escena nueva, guardar, cargar, recompilar shaders, abrir ayuda, ver estado de frame/memoria/profiling y controlar la ventana. |
| **Command Pipeline** | Crear, seleccionar, ordenar, agrupar, habilitar/deshabilitar y perfilar commands. |
| **Resources** | Crear y gestionar recursos GPU y valores editables. |
| **Viewport** | Ver la escena renderizada, pausar/reanudar, reiniciar tiempo/frame y activar pantalla completa. |
| **Log** | Ver errores, warnings y mensajes del motor. |
| **Inspector / General** | Editar propiedades del elemento seleccionado, revisar bindings, configurar cámara, VSync y profiler. |

---

## Qué puedes hacer ahora mismo

### 1. Crear y editar recursos GPU

Puedes crear recursos desde el panel **Resources** con clic derecho.

Recursos soportados:

| Recurso | Uso principal |
|---|---|
| `int`, `int2`, `int3` | Valores enteros editables para parámetros. |
| `float`, `float2`, `float3`, `float4` | Valores escalares/vectoriales para shaders y User CB. |
| `Texture2D` | Texturas cargadas desde disco. |
| `RenderTexture2D` | Render targets 2D con RTV/SRV/UAV/DSV configurables. |
| `RenderTexture3D` | Texturas 3D con SRV/UAV para compute o efectos volumétricos. |
| `StructuredBuffer` | Buffers estructurados para datos GPU, instancing, partículas o argumentos indirectos. |
| `Mesh` | Primitivas internas o meshes glTF/GLB. |
| `Shader` | Shaders VS+PS o compute shaders. |
| Built-ins | Tiempo, color de escena, depth de escena, shadow map y luz direccional. |

También puedes renombrar y eliminar recursos de usuario desde el menú contextual.

---

### 2. Trabajar con valores editables

El editor permite crear valores escalares y vectoriales:

- `int`
- `int2`
- `int3`
- `float`
- `float2`
- `float3`
- `float4`

Estos valores se pueden usar como fuentes para parámetros de shader o para construir un cbuffer de usuario. Sirven para exponer controles rápidos sin tocar código C++.

Ejemplos de uso:

- Radio de blur.
- Intensidad de bloom.
- Color/tint.
- Número de iteraciones conceptual controlado por shader.
- Escalas de material.
- Parámetros de simulación.

---

### 3. Cargar texturas 2D

Puedes cargar texturas desde disco y usarlas como SRV en shaders.

Formatos prácticos soportados por stb_image:

- PNG
- JPG/JPEG
- TGA
- BMP
- HDR

Comportamiento importante:

- Las texturas LDR se suben como `RGBA8`.
- Las texturas HDR se suben como `RGBA32F`.
- Las texturas cargadas se pueden recargar desde el Inspector.
- El selector de path incluye autocompletado y filtrado por extensión en varios campos.

---

### 4. Crear render textures 2D

Puedes crear `RenderTexture2D` con:

- Ancho y alto.
- Formato DXGI.
- Flags de vista:
  - RTV: usable como render target.
  - SRV: legible desde shader.
  - UAV: escribible desde compute o pixel UAV.
  - DSV: usable como depth target.
- Escalado dependiente de la escena.

El **scene scale divisor** permite que una render texture siga el tamaño del viewport:

| Divisor | Comportamiento |
|---:|---|
| `0` | Tamaño fijo. |
| `1` | Tamaño completo del viewport de escena. |
| `2` | Mitad de resolución. |
| `4` | Cuarto de resolución. |

Esto es útil para:

- Post-procesado full-res.
- SSAO/SSGI a media resolución.
- Bloom a baja resolución.
- G-buffers.
- Ping-pong de compute.
- Depth buffers auxiliares.

---

### 5. Crear render textures 3D

También puedes crear `RenderTexture3D` con:

- Ancho.
- Alto.
- Profundidad.
- Formato DXGI.
- SRV.
- UAV.
- RTV cuando el formato y uso lo permitan.

Esto abre la puerta a efectos como:

- Volúmenes.
- Campos 3D.
- Simulaciones en grid.
- Texturas generadas por compute.
- Datos temporales para raymarching o efectos volumétricos.

---

### 6. Crear structured buffers

Los `StructuredBuffer` se crean indicando:

- Stride en bytes.
- Número de elementos.
- Si tendrá SRV.
- Si tendrá UAV.

Sirven para:

- Datos de instancing.
- Sistemas de partículas GPU.
- Buffers de simulación.
- Argumentos indirectos.
- Datos leídos desde vertex shader, pixel shader o compute shader según binding.

El editor muestra el tamaño total estimado del buffer al crearlo o recrearlo.

---

### 7. Usar meshes primitivos

Puedes crear meshes internos sin importar archivos externos:

| Primitiva | Uso típico |
|---|---|
| Cube | Geometría básica, debug, materiales simples. |
| Tetrahedron | Primitiva ligera para tests. |
| Sphere | PBR, iluminación, reflejos, materiales. |
| Fullscreen Triangle | Post-procesado y passes a pantalla completa. |

El `fullscreen_triangle` está pensado para shaders que ya trabajan en NDC y no necesitan multiplicar por matrices de cámara.

---

### 8. Cargar glTF / GLB

Puedes cargar meshes glTF/GLB mediante cgltf.

Capacidades actuales:

- Lee meshes con primitivas de triángulos.
- Usa atributos `POSITION`, `NORMAL` y `TEXCOORD0` cuando están disponibles.
- Importa partes de mesh como subrangos dibujables.
- Importa materiales hasta el límite interno.
- Carga texturas externas y texturas embebidas en buffer views.
- Detecta materiales double-sided.
- Detecta materiales con alpha blend.
- Permite habilitar/deshabilitar partes del mesh desde el Inspector.
- Muestra conteo de vértices, índices, partes y materiales.
- Si falla la carga, crea un cubo fallback y marca el recurso con warning.

Slots de material glTF usados por convención:

| Slot PS | Uso |
|---:|---|
| `t0` | Base color |
| `t1` | Metallic/Roughness o equivalente |
| `t2` | Normal map |
| `t3` | Emissive |
| `t4` | Occlusion |

Notas:

- Las imágenes glTF como `data:` URI no están soportadas todavía y se saltan.
- Sólo se procesan primitivas triangulares.
- Si faltan normales o UVs, se usan valores por defecto.

---

### 9. Compilar shaders HLSL en runtime

Puedes crear dos tipos de shader:

| Tipo | Entry points esperados |
|---|---|
| VS+PS | `VSMain` y `PSMain` |
| CS | `CSMain` |

Características:

- Compilación con Shader Model 5.0.
- Recompilación individual desde el Inspector.
- Recompilación global con botón **Compile** o `F5`.
- Fallback automático si el archivo no existe o falla la compilación.
- Log de errores de compilación.
- Reflexión automática del cbuffer `b1`.
- Sincronización de parámetros de command a partir del layout reflejado.

Registros convencionales:

| Registro | Uso |
|---|---|
| `b0` | `SceneCB`, inyectado por el motor. |
| `b1` | Parámetros propios del shader, reflejados por el editor. |
| `b2` | `ObjectCB`, matriz `World` para draws. |
| `s0` | Sampler lineal. |
| `s1` | Sampler de comparación para sombras. |

---

### 10. Editar parámetros de shader desde la UI

Cuando un shader declara un cbuffer en `register(b1)`, lazyTool intenta reflejar sus variables y exponerlas como parámetros editables en commands que usan ese shader.

Tipos soportados en `b1`:

- `float`
- `float2`
- `float3`
- `float4`
- `int`
- `int2`
- `int3`

Puedes usar parámetros de dos maneras:

1. **Hardcoded:** el valor vive en el command.
2. **Linked:** el parámetro toma su valor desde un recurso `value` compatible.

Esto permite cambiar valores de shader sin recompilar.

Limitaciones relevantes:

- No se reflejan matrices ni arrays en `b1`.
- Los nombres de parámetros deben coincidir exactamente con los nombres en HLSL.
- El packing de HLSL sigue aplicando, así que conviene agrupar variables de forma ordenada.

---

### 11. Construir un User CB global

El panel **User CB (b1)** permite crear un cbuffer de usuario con variables enlazadas a recursos.

Puedes:

- Añadir recursos escalares/vectoriales compatibles al User CB.
- Cambiar nombres de variables.
- Editar valores directos.
- Enlazar cada entrada a un recurso existente.
- Ver un snippet HLSL generado con `packoffset(cN)`.

El User CB usa slots de 16 bytes, estilo `float4`, y se bindea en `b1` para VS/PS/CS.

Esto resulta útil para shaders sencillos o para prototipar parámetros globales.

---

## Commands disponibles

Los commands son los bloques de ejecución del render graph. Se crean con clic derecho en **Command Pipeline**.

| Command | Qué hace |
|---|---|
| `Clear` | Limpia color y/o depth. |
| `Group` | Contenedor lógico para organizar commands. |
| `DrawMesh` | Dibuja un mesh con shader VS+PS. |
| `DrawInstanced` | Dibuja un mesh con instancing. |
| `Dispatch` | Lanza un compute shader. |
| `Repeat` | Repite hijos compute varias veces. |
| `IndirectDraw` | Ejecuta `DrawIndexedInstancedIndirect`. |
| `IndirectDispatch` | Ejecuta `DispatchIndirect`. |

Puedes:

- Activar/desactivar commands.
- Renombrarlos.
- Eliminarlos.
- Reordenarlos con drag & drop.
- Meter commands dentro de grupos.
- Añadir dispatches como hijos de un `Repeat`.
- Ver warnings si faltan bindings o hay estados incompletos.
- Ver tiempos GPU por command cuando el profiler está activo.

---

## DrawMesh y DrawInstanced

Los commands de dibujo permiten configurar:

- Mesh.
- Shader VS+PS.
- Render target principal.
- Depth buffer.
- MRTs adicionales.
- Estado de render.
- Transform.
- Instancias.
- Texturas para pixel shader.
- SRVs para vertex shader.
- UAVs desde pixel shader vía output merger.
- Parámetros de shader.
- Shadow cast.
- Shadow receive.
- Shader opcional para shadow pass.

Estados editables:

| Estado | Qué controla |
|---|---|
| Color Write | Si escribe color. |
| Depth Test | Si usa depth test. |
| Depth Write | Si escribe depth. |
| Alpha Blend | Si usa blending alpha. |
| Backface Cull | Si descarta caras traseras. |
| Shadow Caster | Si participa en el shadow prepass. |
| Shadow Receiver | Si recibe la shadow map en `t7`. |

Transform editable:

- Posición XYZ.
- Rotación XYZ.
- Escala XYZ.

En `DrawInstanced` también puedes cambiar el número de instancias.

---

## MRT: Multiple Render Targets

Los draw commands soportan render target principal más MRTs adicionales hasta el límite interno.

Esto permite montar pipelines como:

- G-buffer deferred.
- Salida simultánea de albedo, normal y world position.
- Masks auxiliares.
- Buffers intermedios para post-procesado.

Límite interno:

- Máximo de 4 render targets activos en draw: RT principal + 3 MRTs adicionales.

---

## Pixel UAV outputs

Los draw commands también pueden escribir UAVs desde pixel shader a través del Output Merger de DX11.

El editor muestra el primer slot UAV válido según cuántos RTVs están activos, porque en DX11 los slots de RTV y UAV comparten espacio en OM.

Uso típico:

- Efectos avanzados desde pixel shader.
- Buffers auxiliares.
- Debug o acumulación controlada.

---

## Dispatch compute

Los `Dispatch` ejecutan compute shaders.

Puedes configurar:

- Compute shader.
- SRVs de entrada.
- UAVs de salida.
- Parámetros reflejados del shader.
- Número de grupos `x y z`.
- Fuente opcional para calcular el tamaño de dispatch.

### Dispatch directo

Si defines `threads X Y Z` sin fuente de tamaño, se usan como grupos de dispatch.

Ejemplo conceptual:

```text
threads 64 64 1
```

El trabajo total depende de `[numthreads(...)]` dentro del shader.

### Dispatch desde tamaño de recurso

Si usas `dispatch_from`, los valores de `threads` pasan a actuar como divisores del tamaño del recurso.

Ejemplo conceptual:

```text
threads 8 8 1
source = una textura 512x512
resultado = Dispatch(64, 64, 1)
```

Esto permite que efectos de post-procesado y compute se adapten al tamaño real del render target.

---

## Repeat para iteraciones compute

`Repeat` permite ejecutar hijos compute varias veces.

Sirve para:

- Jacobi iterations.
- Blur iterativo.
- Simulación de fluidos.
- Ping-pong de buffers.
- Refinamiento progresivo.

Limitación actual:

- `Repeat` sólo re-ejecuta hijos `Dispatch` e `IndirectDispatch`.

---

## Indirect draw / indirect dispatch

lazyTool incluye commands indirectos:

- `IndirectDraw`
- `IndirectDispatch`

Usan un `StructuredBuffer` como buffer de argumentos, junto con un offset.

Esto permite pipelines GPU-driven más avanzados, como:

- Compute que genera argumentos.
- Draw indirect de partículas o instancias.
- Dispatch indirect adaptativo.

Es una feature avanzada: el layout del buffer de argumentos debe coincidir con lo que espera DX11.

---

## Built-in resources

El motor crea recursos internos que puedes usar en bindings o inspeccionar:

| Built-in | Uso |
|---|---|
| Scene Time | Tiempo, delta/frame y datos temporales para animación. |
| Scene Color | Color final/intermedio de la escena. |
| Scene Depth | Depth buffer de la escena. |
| Shadow Map | Depth map generado por el shadow prepass. |
| Directional Light | Luz direccional editable y sus parámetros de sombra. |

El Inspector permite ver previews de Scene Color, Scene Depth y Shadow Map cuando hay SRV disponible.

---

## Iluminación direccional y sombras

lazyTool incluye una luz direccional built-in con:

- Posición.
- Target.
- Dirección derivada.
- Color.
- Intensidad.
- Tamaño de shadow map.
- Near/far de la luz.
- Extents ortográficos.

Puedes orbitar la luz manteniendo `L` sobre el viewport.

Sombras:

- Los commands con **Shadow Caster** participan en el shadow prepass.
- Los commands con **Shadow Receiver** reciben la shadow map en `t7`.
- Puedes usar un shader de sombra custom para el prepass.
- Si no se define shader de sombra custom, se usa el VS de sombra interno.

---

## Cámara y navegación

El editor tiene una cámara FPS editable.

Controles principales:

| Control | Acción |
|---|---|
| RMB | Mouse look. |
| WASD | Movimiento horizontal / forward-back/right-left. |
| Q / E | Bajar / subir. |
| Shift | Movimiento rápido. |
| Ctrl | Movimiento lento. |
| L | Orbitar luz direccional. |

Desde el panel **General** puedes cambiar:

- Activar/desactivar cámara.
- Mouse look.
- Invertir Y.
- Velocidad base.
- Multiplicador rápido.
- Multiplicador lento.
- Sensibilidad del mouse.
- Posición.
- Yaw.
- Pitch.
- FOV.
- Near plane.
- Far plane.
- Reset de cámara.

---

## Viewport runtime

El viewport permite:

- Ver la escena renderizada en tiempo real.
- Pausar/reanudar ejecución.
- Reiniciar la escena desde frame 0.
- Activar pantalla completa del viewport.
- Redimensionar la surface de escena según el layout.
- Mantener visible el último frame al pausar.

Cuando pausas:

- No se ejecutan commands.
- No avanza el tiempo.
- No se limpia/reconstruye la escena.
- La UI sigue siendo editable.

---

## Profiling y estado

El editor incluye profiling GPU con queries de timestamp DX11.

Puedes ver:

- Tiempo GPU total de frame.
- Tiempo GPU del rango de commands.
- Tiempo GPU por command en la lista.
- Memoria de aplicación.
- Estimación de memoria GPU.
- Estimación de memoria usada por recursos de usuario.
- Delta time.
- Número de commands activos.
- Estado de VSync.

El profiler se activa desde **General → Profiler**.

---

## Log integrado

El panel **Log** muestra:

- Mensajes informativos.
- Warnings.
- Errores.
- Errores de compilación de shaders.
- Problemas al cargar texturas o meshes.
- Avisos de fallback.
- Eventos de escena como restart o resize.

También puedes limpiar el log desde el propio panel.

---

## Inspector

El Inspector cambia según la selección.

Para recursos permite editar o ver:

- Valores escalares/vectoriales.
- Tamaño y flags de render textures.
- Formato DXGI.
- SRV/RTV/UAV/DSV.
- Preview de texturas y targets.
- Path de textura, mesh o shader.
- Recarga de textura.
- Recarga de mesh glTF.
- Cambio de primitiva mesh.
- Recompilación de shader.
- Errores de shader.
- Variables reflejadas de cbuffer.
- Partes y materiales glTF.
- Flags GPU del recurso.

Para commands permite editar:

- Enabled.
- Mesh/shader.
- Parámetros de shader.
- Targets.
- MRTs.
- Render state.
- Transform.
- Instancias.
- Bindings de texturas/SRVs/UAVs.
- Dispatch threads.
- Dispatch source.
- Repeat iterations.
- Parent/group.
- Indirect buffer y offset.

La pestaña **Bindings** resume cómo queda conectado el command seleccionado.

---

## Bindings y convenciones de slots

### Draw commands

| Binding | Shader stage | Slots |
|---|---|---|
| `textures` | Pixel Shader | `t0..t7` |
| `srvs` | Vertex Shader | `t0..t7` |
| `uavs` | Pixel Shader / OM | `u0..u7`, con reglas de DX11 OM |

Convención de `t#` en pixel shader:

| Slot | Uso habitual |
|---:|---|
| `t0` | Base color de material glTF. |
| `t1` | Metallic/Roughness. |
| `t2` | Normal map. |
| `t3` | Emissive. |
| `t4` | Occlusion. |
| `t5` | Environment map para PBR/HDRI. |
| `t6` | Libre / user-defined. |
| `t7` | Shadow map cuando Shadow Receiver está activo. |

Los bindings manuales pueden sobrescribir las texturas de material en el mismo slot.

### Dispatch commands

| Binding | Shader stage | Slots |
|---|---|---|
| `srvs` | Compute Shader | `t0..t7` |
| `uavs` | Compute Shader | `u0..u7` |
| Parámetros `b1` | Compute Shader | `b1` |

---

## Shaders incluidos

El repo incluye shaders que cubren varios casos de uso:

| Shader | Qué demuestra |
|---|---|
| `normal_color.hlsl` | Render básico coloreado por normales. |
| `lit_mat.hlsl` | Material iluminado. |
| `lit_ground.hlsl` | Suelo iluminado. |
| `pbr_hdri.hlsl` | PBR con entorno HDRI. |
| `sky_hdri.hlsl` | Sky/background HDRI. |
| `dbg_normals.hlsl` | Debug de normales. |
| `dbg_uvs.hlsl` | Debug de UVs. |
| `dbg_worldpos.hlsl` | Debug de posición mundo. |
| `gbuf_fill.hlsl` | Llenado de G-buffer. |
| `gbuf_resolve.hlsl` | Resolve deferred. |
| `ssao_compute.hlsl` | Cálculo SSAO. |
| `ssao_blur.hlsl` | Blur de SSAO. |
| `ssao_apply.hlsl` | Composición SSAO. |
| `ssgi_compute.hlsl` | Aproximación SSGI. |
| `ssgi_blur.hlsl` | Blur SSGI. |
| `ssgi_composite.hlsl` | Composición SSGI. |
| `bloom_threshold.hlsl` | Extracción de brillo. |
| `blur_h.hlsl` | Blur horizontal. |
| `blur_v.hlsl` | Blur vertical. |
| `bloom_composite.hlsl` | Composición de bloom. |
| `godrays.hlsl` | God rays / efecto volumétrico screen-space. |
| `godrays_apply.hlsl` | Aplicación de god rays. |
| `sdf_scene.hlsl` | Raymarch/SDF. |
| `particles_update.hlsl` | Update de partículas por compute. |
| `particles_draw.hlsl` | Dibujo de partículas. |
| `fluid_advect.hlsl` | Advección de fluido 2D. |
| `fluid_divergence.hlsl` | Divergencia de fluido. |
| `fluid_jacobi.hlsl` | Jacobi para presión. |
| `fluid_gradsub.hlsl` | Resta de gradiente. |
| `fluid_inject.hlsl` | Inyección de fuerza/tinta. |
| `fluid_visualize.hlsl` | Visualización del fluido. |
| `halftone_comic.hlsl` | Look retro/halftone. |
| `triplanar_noise.hlsl` | Material triplanar con ruido. |

---

## Tipos de efectos que puedes montar

Con las piezas actuales puedes construir:

- Render forward simple.
- PBR con HDRI.
- Sky HDRI.
- Sombras direccionales.
- Debug views de normales, UVs y world position.
- G-buffer.
- Deferred resolve.
- SSAO.
- SSGI aproximado.
- Bloom.
- Blur separable.
- God rays / volumétricos screen-space.
- Raymarching SDF.
- Partículas GPU.
- Simulación de fluidos 2D.
- Post-procesado fullscreen.
- Materiales triplanar/noise.
- Halftone/comic shading.
- Pipelines ping-pong entre render textures.
- Compute sobre texturas 2D/3D.
- Instancing con datos en buffers.
- Workflows indirectos GPU-driven.

---

## Atajos de teclado

| Atajo | Acción |
|---|---|
| `Space` | Pausar / reanudar ejecución de escena. |
| `F6` | Reiniciar escena desde frame 0. |
| `F11` | Alternar fullscreen del viewport. |
| `F5` | Recompilar shaders. |
| `Ctrl + S` | Guardar. |
| `F1` | Mostrar/ocultar panel de atajos. |
| Flechas | Navegar por Resources / Commands. |
| `Enter` | Seleccionar elemento enfocado. |
| `F2` | Renombrar recurso o command seleccionado. |
| `Delete` | Eliminar selección. |
| `X` | Activar/desactivar command seleccionado. |
| `RMB` | Mouse look en viewport. |
| `WASD` | Mover cámara. |
| `Q / E` | Bajar / subir cámara. |
| `Shift` | Movimiento rápido. |
| `Ctrl` | Movimiento lento. |
| `L` | Orbitar luz direccional. |

---

## Persistencia básica

La herramienta puede guardar y cargar el estado editable de la escena en archivos de texto `.lt`.

El guardado incluye, entre otras cosas:

- Cámara.
- Luz direccional.
- Recursos de usuario.
- Partes de mesh desactivadas.
- Commands.
- Targets, bindings, render state, transforms y params.

El formato es deliberadamente simple y estricto, pensado para ser legible y fácil de generar. Aun así, este README no documenta en detalle la capa de proyectos/escenas porque queda fuera del alcance actual.

---

## Límites internos importantes

Estos límites están definidos en el motor:

| Límite | Valor |
|---|---:|
| Recursos máximos | 256 |
| Commands máximos | 256 |
| Longitud máxima de nombre | 64 |
| Longitud máxima de path | 256 |
| Texture slots | 8 |
| SRV slots | 8 |
| UAV slots | 8 |
| Render targets por draw | 4 |
| Texturas por material mesh | 5 |
| Partes por mesh | 128 |
| Materiales por mesh | 64 |
| Variables User CB | 64 |
| Variables reflejadas por shader CB | 32 |
| Params por command | 32 |

---

## Limitaciones actuales

- Sólo Windows / DirectX 11.
- Shader Model 5.0.
- Input layout fijo: `POSITION`, `NORMAL`, `TEXCOORD0`.
- Los shaders VS+PS esperan `VSMain` y `PSMain`.
- Los compute shaders esperan `CSMain`.
- El cbuffer reflejado editable es `register(b1)`.
- `b1` sólo soporta escalares/vectores simples, no matrices ni arrays.
- `Repeat` sólo repite dispatches compute, no draws.
- glTF sólo soporta primitivas triangulares.
- Algunas rutas o nombres con espacios pueden ser problemáticos en el formato de texto porque el parser usa tokens separados por espacios/tabulaciones.
- Las texturas glTF en data URI no están soportadas todavía.
- No hay sistema de gizmos 3D todavía; los transforms se editan numéricamente.
- No hay editor visual de nodos; el pipeline se edita como lista/árbol de commands.

---

## Estructura relevante del repo

```text
lazyTool/
├─ assets/
│  ├─ hdri/          # HDRIs de ejemplo
│  ├─ icons/         # Iconos SVG usados por la UI
│  └─ models/        # Modelos de ejemplo
├─ external/
│  ├─ imgui/         # Dear ImGui
│  ├─ stb/           # stb_image
│  ├─ cgltf/         # loader glTF
│  └─ nanosvg/       # iconos SVG
├─ shaders/          # Shaders HLSL
├─ src/              # Motor/editor C++
├─ build.bat         # Script de build MSVC
└─ app.ico / app.rc  # Recursos de aplicación Windows
```

Archivos C++ principales:

| Archivo | Responsabilidad |
|---|---|
| `main.cpp` | Win32, loop principal, cámara, tiempo, built-ins, ejecución frame. |
| `dx11_ctx.cpp` | Inicialización DX11, swapchain, escena, depth, shadow map, estados. |
| `resources.cpp` | Creación/carga/liberación de recursos GPU. |
| `commands.cpp` | Ejecución del pipeline y profiler GPU. |
| `shader.cpp` | Compilación HLSL, fallback y reflexión de cbuffers. |
| `project.cpp` | Serialización/carga textual del estado editable. |
| `ui.cpp` | Interfaz ImGui, inspector, viewport, comandos y recursos. |
| `user_cb.cpp` | Cbuffer de usuario y sincronización de params. |
| `log.cpp` | Log interno. |
| `app_settings.cpp` | Preferencias globales del editor. |

---

## Flujo típico de uso

1. Abre `lazyTool.exe`.
2. Crea o carga recursos en **Resources**.
3. Añade commands en **Command Pipeline**.
4. Selecciona cada command y configura sus targets, shaders, bindings y parámetros en el **Inspector**.
5. Ajusta cámara/luz desde el viewport o el panel **General**.
6. Recompila shaders con **Compile** o `F5` cuando cambies HLSL.
7. Activa el profiler si necesitas medir coste por command.
8. Usa el log para revisar warnings, errores de carga o errores de compilación.

---

## Filosofía de la herramienta

lazyTool prioriza:

- Iteración rápida.
- Control explícito del pipeline.
- Shaders fáciles de probar.
- Recursos GPU visibles e inspeccionables.
- Debug visual inmediato.
- Cero magia innecesaria.

No intenta ocultar cómo se conectan los recursos: el valor está en poder ver y cambiar directamente targets, SRVs, UAVs, estados de render y parámetros.

