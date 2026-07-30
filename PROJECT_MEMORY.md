# lazyTool — memoria técnica viva

Este documento conserva una visión de alto nivel del proyecto y de sus decisiones
técnicas. Debe actualizarse cuando cambien el formato `.lt`, el runtime, los tipos de
recursos/comandos, cualquiera de los dos exportadores o el sistema de build.

## Última revisión

- Fecha: 2026-07-30
- Rama y commit revisados: `main` en `8fe0db2`
- Alcance: arquitectura general, serialización `.lt`, runtime, export normal,
  `build64k`, comando común `Swap` e implementación de skinned meshes en GPU
- Tipo de revisión: lectura amplia del código y documentación, más pruebas mínimas
  de compilación/exportación; no es todavía una auditoría exhaustiva de cada ruta
  de render
- Estado local observado al terminar: `build/build_number.txt` modificado de 26 a
  31 por las compilaciones de validación. Los ZIP
  `dist/lazyTool_build_LT174BIHK-R.zip` y
  `dist/lazyTool_build_LT174BIIR-S.zip` ya existían sin seguimiento y se
  preservaron.

## Resumen del proyecto

lazyTool es un editor experimental de escenas gráficas en tiempo real para Windows,
escrito en C++17 sobre Win32 y Direct3D 11. El modelo es explícito: una lista global
de recursos alimenta una lista ordenada de comandos de render/compute, con parámetros
editables, UserCB, cámara, luz, sombras y timelines.

El proyecto favorece estructuras POD, arrays con límites fijos, handles numéricos y
estado global. Es una elección razonable para una herramienta pequeña, un runtime
predecible y experimentos demoscene. No intentaría convertirlo ahora en una
arquitectura orientada a objetos o ECS: el mayor riesgo actual no está ahí, sino en
la duplicación del contrato de exportación y en la falta de pruebas de conformidad.

El editor y el player normal se compilan como unity builds:

- `src/unity_editor.cpp` incluye el runtime completo, la UI ImGui y `main.cpp`.
- `src/unity_player.cpp` incluye prácticamente el mismo runtime sin UI, bajo
  `LAZYTOOL_PLAYER_ONLY`.
- `build.bat` genera `lazyTool.exe` y el stub `lazyPlayer.exe`.

Esta reutilización del runtime es una fortaleza importante: el export normal tiene
una probabilidad alta de comportarse como el editor porque ambos ejecutan las mismas
implementaciones de recursos, comandos, shaders, UserCB y timeline.

## Mapa de módulos

| Módulo | Responsabilidad observada |
|---|---|
| `src/main.cpp` | Entrada Win32, modo editor/player, bucle de frame, cámara, tiempo y CLI |
| `src/dx11_ctx.cpp` | Device/context/swapchain, targets de escena, estados y SceneCB |
| `src/resources.cpp` | Recursos GPU, primitivas, texturas, glTF, splats y NanoVDB |
| `src/commands.cpp` | Clear, draw, instancing, dispatch, indirect, swap, repeat/grupos y sombras |
| `src/shader.cpp` | Compilación HLSL, reflection y shaders fallback |
| `src/user_cb.cpp` | UserCB global/local, reflection y fuentes dinámicas |
| `src/timeline.cpp` | Clips, tracks, keys, interpolación y aplicación en runtime |
| `src/project.cpp` | Guardado/carga estricta del texto `.lt` |
| `src/embedded_pack.cpp` | Pack autocontenido del export normal |
| `src/ui.cpp` | Toda la aplicación ImGui; actualmente 16.459 líneas |
| `build64k/build64k.cpp` | Parser `.lt`, reducción y generador del player C; 2.443 líneas |

`ui.cpp` es con diferencia el mayor punto de concentración. `resources.cpp`,
`main.cpp` y `commands.cpp` son los siguientes módulos con más responsabilidades.
La unity build no obliga a mantener archivos monolíticos: se podrían separar paneles
y loaders en archivos incluidos por la misma unity TU sin cambiar el modelo de build.

## Formato `.lt`

`project_save_text()` serializa cámara, luz, opciones de export, recursos, audio,
UserCB, comandos y timelines. `project_load_text()` reconstruye directamente el
estado global del editor/runtime.

El loader exige que existan bloques de la forma actual (`export_settings`,
`audio_settings`, `timeline_global` y al menos un `timeline_clip`). Sin embargo, la
línea `lazyTool_project 1` no funciona hoy como un versionado efectivo:

- el writer siempre escribe la versión 1;
- el loader principal no valida esa cabecera;
- el parser 64k tampoco la valida;
- el minificador del export normal elimina la cabecera.

En la práctica, “formato actual” se detecta por presencia y aridad de determinados
campos, no por una versión o esquema único.

También hay dos parsers independientes del mismo formato:

1. `project_load_text()` en `src/project.cpp`;
2. `parse_lt()` en `build64k/build64k.cpp`.

Además, `embedded_pack.cpp` vuelve a interpretar parcialmente el texto para encontrar
assets, audio y defaults eliminables. Cada campo o tipo nuevo puede requerir cambios
en tres lugares, además del writer.

El comando `Swap` se guarda como un comando normal con una línea propia:

```text
swap recurso_a|tipo recurso_b|tipo
```

Su contrato es deliberadamente de bajo nivel: intercambia en O(1) los objetos GPU y
sus vistas, pero mantiene estables nombre, handle y descriptor de cada recurso. Los
operandos deben ser distintos y tener backings, descriptores físicos y política de
resize compatibles. El runtime normal admite render targets 2D/3D, `scene_color`,
`scene_depth` y structured buffers; `build64k` admite RT 2D, ambos built-ins de
escena y structured buffers. Antes del intercambio se desasocian RTV, SRV y UAV
activos para evitar hazards de D3D11.

`scene_color` y `scene_depth` son ahora recursos propietarios reales: sus
`tex/rtv/srv/uav/dsv` viven en su entrada de `g_resources`, no duplicados en
`g_dx`. Los flags `owns_gpu_backing` y `runtime_managed` separan identidad,
ownership y ciclo de vida. `g_dx` conserva el device, swapchain, backbuffer y estado
D3D, mientras draw, compute, previews y presentación resuelven el backing actual
mediante los handles built-in. `shadow_map` también posee ahora su
`tex/srv/dsv` principal en `Resource`; `g_dx` conserva únicamente las vistas
auxiliares por cascada y la SRV de preview, que son estado especializado.

## Skinned meshes evaluadas en GPU

Estado: implementado y validado el 2026-07-30.

Existe un único tipo lógico, `RES_SKINNED_MESH`. El recurso contiene geometría,
índices, parts/materiales, influencias por vértice y el rig estático importado de
glTF. Una pose o conjunto de poses no tiene un tipo especializado: se representa
mediante `RES_STRUCTURED_BUFFER` genéricos escritos por los comandos compute
normales.

Esta separación mantiene como asset únicamente aquello que viene del fichero. El
estado dinámico queda expresado mediante recursos y comandos GPU ya existentes:

```text
SkinnedMesh.rig SRV + parámetros
    -> Dispatch / compute shader
    -> StructuredBuffer genérico de paletas (UAV/SRV)
    -> DrawMesh / DrawInstanced con la paleta enlazada como VS SRV
```

El `SkinnedMesh` es un solo recurso lógico, aunque internamente usa varios objetos
D3D11: VB/IB, SRV de influencias y SRV del rig. Su `buf/srv` genérico expone el rig
estático para que cualquier `Dispatch` lo consuma. Las influencias son un detalle
interno que `DrawMesh`, `DrawInstanced`, sus variantes indirectas y el shadow pass
enlazan automáticamente en el slot VS `t5`. La paleta dinámica se selecciona en la
lista normal de SRV del comando draw; los shaders de ejemplo usan `t6` por
convención, pero el motor no lo reserva.

Para varias instancias, el structured buffer se organiza por bloques:

```text
palette[instance_index * joint_count + joint_index]
```

El vertex shader usa `SV_InstanceID` y `SV_VertexID` para seleccionar,
respectivamente, la paleta de la instancia y las influencias del vértice. Esto
permite una pose, muchas poses o diferentes buffers de pose sin duplicar el asset.

No hace falta un comando especializado `UpdateSkinPose`: `Dispatch` ofrece los
bindings SRV/UAV y dimensiones necesarios. `compute_on_reset` inicializa buffers
desde el rest pose y otros dispatches pueden modificar o reconstruir la paleta
antes del draw. El ejemplo usa un compute de inicialización y otro de simulación.

Para la primera versión se recomienda skinning en vertex shader, no generar un
vertex buffer deformado mediante compute. El `Vertex` actual de 32 bytes
(`POSITION`, `NORMAL`, `TEXCOORD0`) y su input layout pueden conservarse: un
`StructuredBuffer<SkinInfluence>` interno se indexa con `SV_VertexID`, y el
structured buffer genérico enlazado al draw contiene las paletas. Compute skinning
queda como optimización futura si una pose se consume en muchos pases o la
deformación se vuelve más cara.

Contrato implementado:

- glTF/GLB; se importa el primer nodo que contiene `mesh + skin`;
- primitivas de triángulos;
- `JOINTS_0` + `WEIGHTS_0`, hasta cuatro influencias normalizadas por vértice;
- nodos intermedios que no son joints colapsados en el transform local efectivo;
- rest pose local/global, parent index, joint index e inverse-bind conservados;
- sin morph targets, `JOINTS_1`, animaciones glTF ni múltiples nodos skinned en un
  mismo recurso;
- sin límite artificial de 256 joints, más allá de los límites prácticos/API;
- shader de sombras skinned explícito y obligatorio: el shader estático integrado
  no se usa con este tipo para evitar sombras en bind pose.

El ABI GPU de cada joint del rig ocupa 208 bytes:

```text
float4x4 RestLocal
float4x4 RestGlobal
float4x4 InverseBind
uint Parent
uint JointIndex
uint2 Padding
```

Cada influencia interna ocupa 32 bytes: `uint4 Joints` + `float4 Weights`.

El importador glTF normal no se puede reutilizar sin una rama específica: hoy visita
los nodos y pasa su transform global al builder de primitivas, horneándolo en la
geometría. En glTF skinned se ignora el transform del nodo que referencia
`mesh + skin`; el bind-shape, si existe, debe venir premultiplicado en la geometría
o postmultiplicado en las inverse-bind matrices. La paleta se calcula como:

```text
jointCurrentGlobal * inverseBind
```

siguiendo en HLSL la convención actual `mul(M, v)`. El importer no hornea el
transform del nodo `mesh + skin`; el asset debe llegar con su bind shape coherente
con las inverse-bind matrices.

El primer caso de uso previsto es simulación física aproximada de cadenas de huesos,
especialmente vegetación. No se pretende resolver la jerarquía exacta dentro del
mismo frame: cada thread corresponde a un hueso, lee el transform global de su padre
en el frame anterior y escribe exclusivamente el estado actual de su propio hueso.
Esto convierte la dependencia jerárquica en una actualización temporal tipo Jacobi
y evita barreras o recorridos seriales.

Se usarán dos structured buffers genéricos compatibles:

```text
rig estático + bones_prev SRV
    -> Dispatch, un thread por (instancia, joint)
    -> bones_curr UAV
    -> Swap(bones_prev, bones_curr)
    -> DrawMesh / DrawInstanced leyendo bones_prev como VS SRV
```

El retardo de un frame por enlace es intencionado para vegetación y produce
propagación/inercia a lo largo de la cadena. Para cadenas largas o mayor estabilidad
se podrán ejecutar varios substeps por frame, alternando los buffers mediante
`Swap`. El estado dinámico debería incluir al menos transform global y velocidad
angular; la matriz final de skinning puede guardarse en el mismo elemento para que
el vertex shader la consuma directamente.

El export normal lo soporta incluyendo glTF/GLB y dependencias igual que el mesh
estático. `build64k` reconoce `skinned_gltf` como recurso externo no soportado y
avisa al omitir tanto la geometría como los comandos que dependen de su SRV; todavía
no hace un rechazo estricto global. Un futuro soporte 64k requerirá un bake offline
de geometría, influencias y esqueleto a arrays generados.

La fixture `projects/skinned_tree_gpu.lt` y el asset
`assets/models/skinned_tree.gltf` forman el ejemplo mínimo. El rig tiene cuatro
huesos y el estado dinámico ocupa 144 bytes por hueso: global, matriz de skinning y
un `float4` de dinámica. El solver añade una oscilación amortiguada dependiente del
viento, toma la global del padre del frame anterior y escribe solo el joint propio.
El ejemplo incluye shaders separados de init, simulación, draw y sombras.

## Ruta 1: export normal desde el editor

Flujo:

```text
estado del editor
  -> project_save_text(.lt)
  -> collect_project_refs()
  -> minificación de .lt/HLSL
  -> copia de lazyPlayer.exe
  -> pack de proyecto + assets + footer añadido al PE
  -> mismo loader/runtime que usa el editor
```

Características:

- La UI guarda el proyecto antes de exportar, evitando empaquetar estado obsoleto.
- Se usa un player dedicado y se comprueba el marcador
  `LAZYTOOL_PLAYER_STUB_V2`.
- Se incluyen shaders e includes, texturas, glTF/GLB y sus ficheros externos,
  Gaussian splats y NanoVDB.
- Los assets binarios se preservan; el proyecto y HLSL se minifican.
- El player carga el pack al arrancar y `lt_read_file()` hace que el resto del
  runtime pueda seguir usando rutas de fichero normales.
- Las opciones de cámara/luz, timeline, salida al final, Escape, VSync y título FPS
  viven en el proyecto.
- `Swap` usa la misma implementación de recursos en editor y player normal, por lo
  que conserva la paridad de esta ruta.

Valoración: la idea es simple y sólida. Compartir el runtime es exactamente lo que
conviene para el export de fidelidad completa.

Riesgos observados:

- `ExportList` tiene capacidad fija para 512 ficheros, pero varios callers ignoran
  el `false` de `export_add_file()`. Una escena con muchos includes o dependencias
  glTF puede producir un EXE incompleto sin fallar durante la recolección.
- La recolección de includes es textual, tiene profundidad máxima 8 y no propaga
  errores de includes ausentes hasta fases posteriores.
- El minificador usa buffers de línea de 1.024 bytes para `.lt` y 2.048 para HLSL.
  Las líneas más largas se truncan silenciosamente y podrían corromper un shader
  válido o notas largas.
- La lista de líneas/defaults eliminables está codificada mediante comparaciones de
  strings exactas. Cambiar un default o su formato puede introducir divergencia.
- La CLI `--export` empaqueta texto sin hacer un preflight completo con el loader;
  la UI es más segura porque primero guarda desde un estado válido.
- Los fallos después de crear el destino dejan un EXE parcial. Conviene escribir a
  un temporal, validar footer/proyecto y renombrar al final.
- El pack completo se carga en RAM y cada lectura crea otra copia. Para splats o
  NanoVDB grandes puede haber un pico de memoria considerable.
- No hay checksum del contenido. No es imprescindible para un pack local, pero una
  validación barata detectaría corrupción con mejores diagnósticos.

## Ruta 2: `build64k`

Flujo:

```text
proyecto .lt guardado
  -> parser C++ independiente
  -> Project intermedio propio
  -> filtrado y flattening de recursos/comandos/timelines
  -> generación de out64k.c
  -> MSVC C17 con /NODEFAULTLIB
  -> UPX sobre lt64k.exe
```

Características verificadas:

- Genera un runtime Win32/D3D11 independiente, no una variante del player normal.
- Embebe HLSL expandiendo includes y compila shaders al arrancar.
- Soporta clears, draws procedurales, primitivas internas, targets internos,
  profundidad/sombras, parámetros, UserCB, timeline, audio GPU y una ruta compute
  directa con buffers/UAV.
- Emite `Swap` como códigos enteros de recurso y cambia directamente los arrays de
  objetos/vistas del runtime generado, sin copiar contenido GPU.
- Si se usa `scene_color` como SRV/UAV o en un swap, genera un target de escena
  intermedio y lo copia al backbuffer al presentar; los demás proyectos mantienen
  la ruta directa. Los depth RT/DSV internos también se emiten solo cuando hacen falta.
- Filtra shaders y structured buffers no usados.
- Aplana `Repeat` y serializa datos a arrays compactos.
- El código generado especializa partes del runtime según las features usadas.
- Contenido externo no procedural se avisa y se omite.

Valoración: la separación está justificada. Un runtime mínimo generado puede quitar
mucho más código y datos que el player normal. El `Project` intermedio del exportador
es una buena dirección, pero debería convertirse en un contrato compartido o
generado, no seguir evolucionando como un segundo parser manual.

Riesgos y diferencias:

- Contenido o comandos no soportados se saltan con warnings y el proceso puede
  terminar con éxito, incluso con un player parcial o sin comandos. Para un export
  final debería existir un modo estricto por defecto.
- Hay un bug probable con `Repeat` y tracks/fuentes por nombre: el flattening crea
  varias copias del hijo con el mismo nombre y `cmd_out_index[name]` conserva solo
  el último índice. Un track `cmd_transform`/`cmd_enabled` o una fuente basada en
  comando afectaría solo a la última copia, no a todas las iteraciones como en el
  runtime normal.
- Los render targets internos no pasan por la misma poda de liveness que shaders y
  buffers; los UserCB se conservan todos. Hay margen claro de reducción adicional.
- Recursos desconocidos para el parser pueden desaparecer sin un diagnóstico tan
  preciso como el loader principal. NanoVDB, por ejemplo, no tiene categoría propia
  en el IR 64k.
- El parser basado en whitespace no soporta bien nombres o rutas con espacios.
- `LT64K_MINIFY_HLSL` existe pero el build normal del exporter lo deja a 0 para
  priorizar paridad. Es una decisión razonable, aunque debe quedar explícita en el
  informe de tamaño.
- `out64k.c` está versionado pero `build.bat` lo sobrescribe. Si es una fixture
  golden debería tener otro nombre y una prueba de actualización; si es solo un
  artefacto generado, debería salir del control de versiones.

## Comparación de exportadores

| Aspecto | Export normal | `build64k` |
|---|---|---|
| Objetivo | Fidelidad y compatibilidad | Tamaño y demoscene |
| Runtime | Comparte runtime con editor | Runtime C generado |
| Entrada | Estado guardado a `.lt` | `.lt` leído desde fuera |
| Assets externos | Sí | No, salvo fuente HLSL |
| Compute/indirect | Runtime completo | Compute directo limitado; no indirect general |
| Swap | RT 2D/3D, scene color/depth y structured buffers | RT 2D, scene color/depth y structured buffers |
| Fallo ante incompatibilidad | Falla sobre todo por ficheros/IO | Suele avisar y omitir |
| Riesgo principal | Recolección/minificación textual | Deriva semántica respecto al runtime |
| Optimización | Stub dedicado + texto minificado | Feature stripping + arrays compactos + linker + UPX |

## Hallazgos priorizados

### Prioridad alta

1. Crear un contrato único del formato: versión real, tokenizer/parser compartido y
   un `ProjectIR` independiente del estado global.
2. Añadir preflight/manifest a ambos exports. `build64k` debería fallar por defecto
   ante contenido omitido y ofrecer `--allow-partial` solo de forma explícita.
3. Corregir y cubrir con test la semántica `Repeat` + tracks/fuentes de comando.
4. Hacer que la recolección normal propague overflow, include/dependencia no resuelta
   y truncamientos; nunca generar silenciosamente un pack parcial.

### Prioridad media

5. Crear fixtures de conformidad que pasen por writer, loader normal y parser 64k.
6. Sustituir el minificador textual de defaults por una serialización compacta desde
   el IR o, como mínimo, tests golden por versión de formato.
7. Escribir exports a temporal, reabrirlos/validarlos y hacer rename atómico.
8. Añadir liveness de render targets y UserCB al 64k, más informe por feature/recurso.
9. Dividir `ui.cpp` por paneles y `resources.cpp` por familias de loaders, manteniendo
   la unity build si sigue siendo útil.

### Higiene/documentación

10. `build64k/build.bat` usa por defecto
    `projects/procedural_spheres_pbr_post.lt`, que no existe en el checkout actual.
11. La documentación menciona `lt64k_unpacked.exe`, pero el script comprime
    `lt64k.exe` in-place y no crea esa copia.
12. La documentación dice que compute/UAV general no está soportado, pero el código
    ya implementa un subconjunto de dispatch directo, structured buffers y UAV.
    Hace falta una matriz precisa de “soportado / limitado / rechazado”.
13. `projects/` está vacío en el checkout revisado. La única fixture `.lt` visible es
    `build64k/clear_only_test.lt`.
14. No se encontraron tests automatizados ni configuración CI en el repositorio.

## Qué cambiaría primero

La secuencia recomendada evita una reescritura grande:

1. Añadir fixtures y una herramienta de preflight que produzca un informe común.
2. Introducir una versión de formato efectiva y un `ProjectIR` parseable sin GPU/UI.
3. Migrar `project_load_text()` y `build64k` al mismo parser/IR.
4. Hacer estricta la elegibilidad 64k y resolver `Repeat`.
5. Endurecer/validar el pack normal.
6. Después optimizar poda y tamaño; finalmente dividir los módulos grandes.

No priorizaría cambiar los arrays fijos, handles o estado global salvo que aparezcan
límites reales. Son coherentes con el objetivo del proyecto y ayudan a que el runtime
sea directo. Sí documentaría los límites como parte del contrato del IR.

## Matriz mínima de pruebas recomendada

| Fixture | Export normal | 64k | Qué cubre |
|---|---:|---:|---|
| clear-only | Sí | Sí | Player sin shaders/timeline activa |
| draw procedural | Sí | Sí | VS/PS, SV_VertexID y SceneCB |
| primitiva + sombras | Sí | Sí | VB generado y shadow fallback |
| timeline multi-clip | Sí | Sí | Interpolación y salida/loop |
| repeat + track de comando | Sí | Sí | Paridad del flattening |
| compute + RT UAV | Sí | Sí limitado | Dispatch directo y bindings |
| structured buffer | Sí | Sí limitado | SRV/UAV y dispatch_from |
| swap RT + buffer + built-ins | Sí | Sí | Ownership, compatibilidad y swap O(1) |
| audio shader | Sí | Sí | Generación y playback |
| glTF + texturas | Sí | Debe fallar estricto | Dependencias externas |
| splat/NanoVDB | Sí | Debe fallar estricto | Assets grandes no procedurales |
| paths/includes largos | Sí | Según contrato | Robustez del parser/pack |

## Validaciones ejecutadas en esta revisión

- Lectura de documentación, build scripts y módulos centrales.
- Trazado de `project_save_text()`/`project_load_text()`.
- Trazado completo del pack/footer y arranque del player normal.
- Trazado del parser, filtrado, flattening y codegen 64k.
- Export normal de `clear_only_test.lt`:
  `clear_normal.exe`, 546.650 bytes, salida correcta.
- Compilación aislada de `build64k.cpp`.
- Generación y compilación C17 de la misma fixture:
  `clear_out64k.c`, 6.892 bytes; player sin comprimir, 4.096 bytes.
- Compilación aislada de editor y player después de integrar `Swap`.
- Export y ejecución real auto-terminada de `build64k/swap_test.lt` en el player
  normal: `builtin_swap_normal.exe`, 654.453 bytes, código de salida 0.
- Generación y compilación C17 de la fixture `swap_test.lt`, con seis comandos,
  cuatro render targets, dos structured buffers y swaps de `scene_color` y
  `scene_depth`: `builtin_swap_out64k.c`, 58.362 bytes; player sin comprimir,
  20.992 bytes.
- Regresión `clear_only_test.lt` recompilada tras la especialización:
  `clear_regression.c`, 6.906 bytes; player sin comprimir, 4.096 bytes.
- La fixture `swap_test.lt` queda en el repositorio para cubrir el
  empaquetado/formato normal y el parsing/codegen de la ruta 64k.
- Compilación completa del editor/player tras integrar `RES_SKINNED_MESH`.
- Compilación aislada con `fxc` de las entradas VS/PS/CS de los cinco shaders
  skinned del ejemplo.
- Carga y ejecución de `projects/skinned_tree_gpu.lt` en el player, con captura de
  `OutputDebugString`: proyecto cargado y sin errores de importer, shader o
  validación D3D11.
- Export normal de esa fixture a `%TEMP%/lazytool_skinned_tree_example.exe`,
  1.462.077 bytes; export correcto y player exportado ejecutándose.
- Parse/codegen 64k de la fixture: el generador avisó explícitamente de que el
  recurso skinned es externo y omitió los dispatches/draw dependientes.
- Los binarios y fuentes C generados durante las pruebas se escribieron en
  `%TEMP%/lazytool_codebase_review` y
  `%TEMP%/lazytool_swap_validation_20260730` y
  `%TEMP%/lazytool_builtin_swap_validation_20260730`, no en el repo.

No se validó visualmente la deformación: el smoke test confirma importación,
compilación y ejecución sin errores, pero la captura offscreen de la swapchain D3D11
resultó negra y no sirve como comparación de imagen.

## Protocolo de actualización

En cada revisión futura:

1. Actualizar fecha, commit y estado local.
2. Registrar recursos/comandos/tracks nuevos y su soporte en ambos exports.
3. Revisar si cambió el formato `.lt` o sus defaults.
4. Ejecutar la matriz de fixtures disponible.
5. Mover hallazgos resueltos al changelog y repriorizar los restantes.
6. Añadir decisiones explícitas; no borrar contexto histórico útil.

## Historial

### 2026-07-30

- Creación de la memoria técnica.
- Primera revisión general de arquitectura y de las dos rutas de exportación.
- Confirmadas ambas rutas con la fixture clear-only.
- Añadido el comando común `Swap` para ping-pong O(1), con validación de
  descriptores, UI, serialización, render graph y soporte en ambos exportadores.
- Unificado el ownership normal de `scene_color` y `scene_depth` dentro de
  `Resource`; eliminados sus punteros GPU duplicados de `g_dx`.
- Movido también el backing principal `tex/srv/dsv` de `shadow_map` a `Resource`;
  el renderer conserva solo sus vistas auxiliares por cascada/preview.
- Añadido Swap de `scene_color`/`scene_depth` con RTs scene-sized compatibles en
  el runtime normal y en `build64k`, con feature stripping del target intermedio.
- Añadida y validada `build64k/swap_test.lt`.
- Corregido el caso `build64k` de runtime completo sin shaders, que necesitaba
  incluir `d3dcompiler.h`.
- Identificados como temas centrales el contrato `.lt` duplicado, la necesidad de
  preflight estricto y la paridad del flattening de `Repeat`.
- Documentado y refinado el diseño propuesto para glTF skinned meshes: un único
  recurso lógico de asset, poses expresadas como structured buffers genéricos,
  actualización mediante `Dispatch` y skinning en VS mediante `SV_VertexID` /
  `SV_InstanceID`.
- Fijado como primer modelo de simulación un solver temporal por hueso para
  vegetación: cada thread lee el padre del frame anterior, escribe su propio estado
  actual y los buffers `prev/curr` se alternan con el comando `Swap`.
- Implementado `RES_SKINNED_MESH`, importación glTF/GLB de geometría, rig e
  influencias, serialización/UI/reload/pack normal y binding automático de
  influencias en VS `t5`.
- Integrado el tipo en draw normal, instancing, indirect y sombras; los skinned
  meshes exigen un shader de sombras explícito.
- Añadidos el asset y proyecto de ejemplo `skinned_tree_gpu`, con init GPU,
  simulación temporal de una cadena de cuatro huesos, ping-pong mediante `Swap`,
  skinning en VS y shadow shader.
- Verificados build completo, shaders con `fxc`, ejecución del proyecto y
  exportación normal. La ruta 64k reconoce el recurso y avisa/omite porque no
  incorpora meshes externos.
