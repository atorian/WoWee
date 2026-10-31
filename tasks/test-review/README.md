# Ревью тестов: поведение или реализация

2026-10-31, ветка `claude/test-review-report-1c484c` (от `3ce37177`).

Мерило: раздел "Tests test behaviour" в `AGENTS.md` и решение от 2026-10-31
о том, что код GPU проверяется на GPU. Прочитаны все 374 файла в `tests/`
(около 3300 `TEST_CASE`). Ничего не собиралось и не запускалось, поэтому
выводы о том, что тест "не может упасть", получены чтением кода, а не
мутацией. Каждую находку из списка "проверено вручную" ниже я перепроверил
сам, grep'ом или по коду. Остальное взято из подробных разборов
`batch-0.md` ... `batch-6.md` в этой папке (на английском, с file:line,
цитатами и предложенной заменой для каждой находки).

## Итог

| Вердикт | Файлов |
|---|---|
| OK | 226 |
| мелкие замечания | 73 |
| есть проблемные тесты | 52 |
| файл целиком удалить или переписать | 23 |

Большая часть набора написана правильно. Тесты через `GameHarness`
(`test_chat_domain`, `test_weather_domain`, `test_loot_domain`,
`test_trade_domain` и другие *_domain), изолированные тесты контекстов
(`test_trade`, `test_who`, `test_mailbox`, `test_pet`) и парсеры с байтами,
собранными по серверному порядку, проверяют поведение и ловят реальные
ошибки. Нарушений изоляции контекстов (категория H) не нашлось ни одного:
каждый тест контекста линкует только свою библиотеку.

Проблемы сосредоточены в нескольких классах, и почти все они живут в
`rendering`, `pipeline` и старых тестах, написанных до правил AGENTS.md.

## Классы проблем

Число в скобках означает примерное количество находок по всем частям.

### 1. Шейдер на CPU и копии продуктовой логики в тесте (D, ~43)

Самый серьёзный класс. Такой тест проверяет копию, а копия расходится с
продуктом, и тест остаётся зелёным.

Расхождение, которое уже случилось (проверено вручную):

- `test_world_map.cpp:97` держит свою копию `renderPosToMapUV` со сдвигом
  континента −0.15. В продукте сдвига нет
  (`src/rendering/world_map/coordinate_projection.cpp:74`, `(void)isContinent`).
  Рядом `test_world_map_coordinate_projection.cpp:71` называется
  "continent applies vertical offset" и проверяет, что сдвига нет. Оба
  теста проходят.
- `test_indoor_shadows.cpp:20` проверяет две собственные функции. Продукт
  уже другой: `renderer.cpp:961` пишет в `shadowParams.y` ноль, а в
  `wmo.frag.glsl` нет ветки `isInterior`.

Шейдерный код, скомпилированный как C++. 13 файлов подключают `.glsli`
напрямую: `test_distance_fog`, `test_distant_terrain` (один случай),
`test_interior_light`, `test_ocean_fft`, `test_ocean_spectrum`,
`test_rt_light_composition`, `test_sea_bed`, `test_shadow_plane`,
`test_shadow_shading`, `test_shelter_probe`, `test_sphere_map`,
`test_volumetric_fog_math`, `test_wind_field`. Ещё шесть переписывают
шейдер руками: `test_grass_cull_math` (копия уже разошлась с шейдером по
радиусу и центру сферы), `test_minimap_projection`, `test_local_lights`,
`test_m2_texture_transform`, `test_sky_celestial` (`cloudLighting`),
`test_rt_bvh` (`traceRtClosest`). Коммит `bc575f3b` уже убрал CPU-копию
кильватерного следа. С этими файлами нужно сделать то же самое: сцена в
`scene_render` и проверка картинки.

Копии CPU-логики в тесте:

- `test_override_add.cpp:51` содержит `resolveLikeAssetManager`, копию
  `AssetManager::resolveFile`. Тест объясняет копию тем, что
  `AssetManager` здесь не собрать, но `test_rain_audio.cpp:137` создаёт
  настоящий (проверено).
- `test_equipment_set_packets.cpp:53` содержит свою копию разбора
  `SMSG_EQUIPMENT_SET_LIST`. Настоящий
  `InventoryHandler::handleEquipmentSetList` не вызывается, и ни один тест
  через `GameHarness` этот пакет не покрывает (проверено).
- `test_taxi_map_projection.cpp:42`: копия цепочки `taxiNodeMapPos`.
- `test_settings_panel_layout.cpp:97`: Lua-раскладка панели настроек,
  переписанная на C++.
- `test_achievement_criteria.cpp:90`, `test_m2_cull_grid.cpp:42`,
  `test_link_hit.cpp:125`, `test_character_session`, `test_env_flag.cpp:74`,
  `test_widget_tree.cpp:935`: оракул в тесте повторяет код продукта.

### 2. Тесты мёртвого кода (G, ~26)

Функция существует только ради теста. По `AGENTS.md` такой код удаляется
вместе с тестом. Проверено `git grep` по всему репозиторию без `tests/`:
вызовов нет ни у одной из этих функций.

| Тест | Функция без вызовов |
|---|---|
| `test_buff_bar_layout` | `computeBuffBarLayout`, `minimapReservedWidth` |
| `test_chat_markup_parser` | `ChatMarkupParser` |
| `test_cata_movement`, большая часть `test_bit_packet` | `readCataMovement` и битовый API под него |
| `test_asset_token_name` | `assetNameHasToken`, `assetNameHasWordToken` |
| `test_local_lights` | `localLightAttenuation` |
| `test_movement_limits`, `test_terrain_slope_limit` | `isWalkableNormal`, `isReachableStep` |
| `test_m2_material` | `m2TextureWeightOpacity` |
| `test_m2_color_track` | `m2ColorRecordSize`, `m2ColorAlphaTrackOffset` |
| `test_rt_bvh` | `traceRtClosest` |
| `test_realm_patches.cpp:143` | `parsePatchManifest` |
| `test_frustum.cpp:25` | `Frustum::containsPoint` |
| `test_dbc_loader.cpp:236` | `DBCFile::getStringByOffset` |
| `test_sun_direction` | `sunDirectionFromLightDir` |
| `test_shared_rules.cpp:371` | `itemQualityColorHexRGB` |

По данным агентов (вручную не проверял), тестовые бэкдоры `setState` в
`test_activity_fsm` и `test_combat_fsm`, а также
`m2UVTranslation`/`m2ApplyUVTransform` в `test_m2_texture_transform` тоже
вызываются только из тестов.

Отдельно про `test_terrain_slope_limit`. Ограничение уклона в игре сейчас
выключено намеренно, причина записана в комментарии
`camera_controller.cpp:1015-1028` (`(void)nz;`). Тест проверяет функцию,
которую игра не вызывает, и поэтому по зелёному тесту не видно, что
ограничения уклона в игре нет.

`setUseHashedK` из `test_srp` вызывается в `tools/auth_login_probe`. Это
код утилиты, не мёртвый код.

Статус на 2026-10-31: всё из таблицы удалено вместе с тестами. С
`readCataMovement` ушли битовый API в `Packet` (и `flushBits`/
`resetBitReader` в его байтовых путях), `movement_sequences.json` и
`tools/derive_cata_movement.py`; `docs/plan-cataclysm.md` называет коммиты
`1af5d99e` и `0a37a272`, откуда их вернуть. Единственный живой случай из
`test_bit_packet`, побайтная раскладка писателей, перенесён в `test_packet`.
Удалено и то, что держалось только на удалённом: весь ImGui-стек разметки
чата (`ChatMarkupRenderer`, `ItemTooltipRenderer`, `CachedChatLine`),
`InventoryScreen::getItemIcon` и `openInterfaceQuestLog` (их нашёл
`tools/dead_symbol_check.py`), `PatchManifest`/`PatchEntry` с лимитами,
`RtHit`, константы затухания и `kMaxWalkableSlopeDegrees`. Тесты, у которых
удалённая функция была лишь частью, оставлены на живом API:
`test_frustum` проверяет точку через `intersectsSphere(p, 0)`, `test_rt_bvh`
проверяет структуру дерева без CPU-обхода.

### 3. Константы, переписанные в тест (A, ~52)

Тест падает, только если кто-то поменял константу, и тогда его правят
вместе с ней.

- Целиком: `test_chat_types` (значения `ChatType`), `test_npc_flags` (все
  `NPC_FLAG_*`), `test_movement_flag_bits` (биты `MovementFlags` и состав
  `kLocomotionFlags`), `test_grass_blade_layout` (`sizeof`/`offsetof`, которые
  и так стоят в `static_assert` в `grass_blade.hpp`).
- Частично: `test_animation_ids.cpp:149`, `test_light_bands.cpp:148-280`
  (7 из 11 тестов), `test_inventory_slots.cpp:18`, `test_dbc_layouts`,
  `test_tile_size_agreement.cpp:24` (`TILE_SIZE`, `ZEROPOINT`, `PI`),
  `test_movement_limits.cpp:7`, `test_m2_color_track.cpp:51`,
  `test_vertex_layouts.cpp:76` и `:134` (`offsetof == offsetof`),
  `test_entity.cpp:373`, `test_zone_tiles`, `test_world_map_zone_metadata`,
  `test_framexml_takeover.cpp:48` (число элементов 52).

Если значение должно совпадать с сервером, правило AGENTS.md такое:
подать пакет или строку DBC с этим значением и проверить, что за этим
следует.

### 4. Тавтологии (B, ~52)

Ожидаемое значение получено из кода под тестом, или тест читает обратно
только что записанное.

- `test_calendar_packet.cpp:38` собирает пакет по константам самого парсера
  (`CalendarHoliday::kNumDates` и других). Ошибку "26 дат", про которую
  говорит комментарий в тесте, этот тест поймать не может (проверено).
- `test_reputation_standing.cpp:15`: ожидаемые значения берутся из
  `kReputationStandings`, то есть из таблицы под тестом.
- `test_movement_payload_layout.cpp:66`: каждый парсер проверяется своим же
  писателем, и одинаковая ошибка в обоих проходит.
- `test_entity.cpp` (~15 случаев) и `test_m2_structs.cpp:92-248` (12 из 16):
  set-then-get и значения по умолчанию.
- `test_world_map_exploration_state.cpp:28`, `test_anim_capability`,
  `test_widget_tree.cpp:584`, `test_widget_tree.cpp:1862` (по сути тест
  `std::vector`).
- `test_transport_components.cpp:66` и `:121` выставляют руками около 40
  полей `ActiveTransport` и сверяют результат с формулой реализации.

### 5. Внутренности вместо входа (C, ~37)

- Регулярки по тексту исходников вместо запуска кода:
  `test_settings_apply_on_load` (весь файл), `test_pom_quality_one_scale.cpp:67`
  (хотя есть `clientSettingsSchema()`), `test_catalog_entry_key`,
  `test_format_lookup`, `test_settings_schema_consistency`,
  `test_update_field_indices` (свой разбор JSON вместо `UpdateFieldTable`).
- `test_framexml.cpp:82+`: около 70 случаев ищут подстроки в
  сгенерированном Lua (`__w[1]`, `"5.000000, -7.000000"`). Ни один из них не
  запускает этот Lua и не проверяет получившиеся фреймы.
- `test_world_socket`: `friend struct WorldSocketTestAccess` пишет приватные
  поля и вызывает приватные шаги разбора. Тест через loopback (`:98`)
  показывает, что можно обойтись без этого.
- Тесты-бэкдоры через `setState` в `test_combat_fsm` и `test_activity_fsm`;
  `test_movement_domain` и пара случаев в `test_combat_domain`,
  `test_session_domain`, `test_spell_domain` подают готовые `UpdateBlock` в
  обход декодера пакетов.

### 6. Тесты, которые не могут упасть (F, ~67)

- Зелёный тест без данных. `Data/db` не лежит в git, поэтому
  `test_dbc_layout_columns.cpp:97` пишет `WARN` и выходит, и в любом чистом
  checkout тест "проходит", ничего не проверив (проверено). То же в
  `test_framexml.cpp:286`, `test_asset_manifest_load`, `test_blp_alpha_scan`,
  `test_m2_color_alpha`, `test_m2_footstep_events`, `test_m2_ribbon_tracks`,
  `test_spell_icon_columns`. Нужен `SKIP(...)`, чтобы пропуск был виден в
  отчёте.
- `REQUIRE_NOTHROW` на загрузчиках, которые не бросают исключений:
  `test_adt_truncation.cpp:64` и `:153`, `test_blp_truncation.cpp:74`. Тест
  "rejects random bytes" не проверяет, что байты отвергнуты.
- Нет утверждений: пять тестов `validateAgainstDBC` в
  `test_animation_ids.cpp:84-147`, тест обрезки в
  `test_gameobject_query_layout.cpp:174`.
- Только отрицательные случаи: в `test_gather_spells` проходят и
  `return false`, и `return nullptr`.
- `test_srp`: нет вектора с известным ответом. Тест проверяет размеры, что
  A ненулевое и что выходы различаются, но ни разу не проверяет, что
  `verifyServerProof` принимает правильное доказательство.
- `test_widget_tree.cpp:2978` назван по ошибке реаллокации, но не создаёт
  виджеты во время обхода раскладки, поэтому эту ошибку поймать не может.
- `test_combat_fsm.cpp:103` (случай плавания) не может упасть.

## Файлы, которые стоит удалить или переписать целиком

| Файл | Почему | Что вместо |
|---|---|---|
| `test_world_map.cpp` | копии функций, умолчания структур, `std::sort`; копия UV уже неверна | ничего, `test_world_map_coordinate_projection` покрывает настоящую функцию |
| `test_indoor_shadows.cpp` | две свои функции, продукт от них ушёл | сцена в `scene_render`, если поведение ещё нужно |
| `test_override_add.cpp` | копия `resolveFile` | те же случаи на настоящем `AssetManager` |
| `test_equipment_set_packets.cpp` | копия парсера | `GameHarness`: `SMSG_EQUIPMENT_SET_LIST` на вход, состояние и события на выходе |
| `test_taxi_map_projection.cpp` | копия `taxiNodeMapPos` | вызов настоящей функции или Lua API |
| `test_placement_rotation.cpp` | без продуктового кода, описанного пути больше нет | ничего, `test_placement_transform` это покрывает |
| `test_distance_fog.cpp`, `test_ocean_fft.cpp`, `test_rt_light_composition.cpp`, `test_shadow_plane.cpp`, `test_shadow_shading.cpp`, `test_shelter_probe.cpp`, `test_sphere_map.cpp` | шейдер на CPU | `scene_render` + `check_*.py` |
| `test_chat_types.cpp`, `test_npc_flags.cpp`, `test_movement_flag_bits.cpp`, `test_grass_blade_layout.cpp` | константы | пакет со значением через `GameHarness`; для раскладки хватает `static_assert` |
| `test_anim_capability.cpp` | умолчания и тривиальная фабрика | ничего |
| `test_buff_bar_layout.cpp`, `test_chat_markup_parser.cpp`, `test_cata_movement.cpp`, `test_terrain_slope_limit.cpp` | мёртвый код | удалить вместе с кодом |
| `test_settings_apply_on_load.cpp` | регулярки по исходникам | загрузить файл настроек и прочитать применённые значения |

## Что сделано хорошо и годится как образец

- `test_chat_domain`, `test_weather_domain`, `test_loot_domain`,
  `test_group_domain`, `test_trade_domain`: пакет на вход, на выходе
  состояние, события аддонов с аргументами, строки чата, ответный пакет и
  проверка, что после релога ничего не осталось.
- `test_trade`, `test_who`, `test_mailbox`, `test_pet`, `test_petitions`:
  контекст отдельно от игры, настоящий `EventBus`, шлюз-фейк только
  записывает отправленное.
- `test_placement_transform`: оракул из данных Blizzard (мировые границы в
  MODF), который не повторяет реализацию.
- `test_battlefield_status_packet`, `test_char_enum_layout`,
  `test_item_query_sockets`, `test_spell_damage_log_layout`: байты собраны
  по серверному писателю с литералами, а не клиентским писателем.
- `test_collision_world`, `test_dxt_block`, `test_realm_patch_sync`,
  `test_texture_cache`, `test_uniform_slot_pool`: публичный API, независимый
  оракул, проверяются исходы, а не журнал вызовов.

## Порядок работы, который я бы предложил

1. Удалить мёртвый код вместе с тестами (класс 2). Это механическая работа,
   и после неё список проблемных файлов заметно сократится.
2. Заменить `WARN`/`SUCCEED` + `return` на `SKIP` во всех тестах на данных.
   Сейчас в отчёте они выглядят как пройденные.
3. Убрать копии CPU-логики: `test_world_map`, `test_override_add`,
   `test_equipment_set_packets` (через `GameHarness`),
   `test_taxi_map_projection`, `test_calendar_packet`.
4. Перенести 19 шейдерных тестов на `scene_render`. Это самая большая
   часть, и коммит `bc575f3b` уже сделал так для кильватера.
5. Удалить тесты констант и тавтологии; где значение важно для протокола,
   заменить пакетным тестом.

Перед удалением конкретного теста стоит проверить его мутацией, как требует
AGENTS.md. В этом ревью я этого не делал.
