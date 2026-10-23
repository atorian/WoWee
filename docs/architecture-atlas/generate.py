#!/usr/bin/env python3
"""Rebuild the offline architecture atlas. Requires Python 3 and Graphviz dot.
Run from any directory. All outputs and temporary files stay on drive D.
"""
from pathlib import Path
from html import escape as h
import hashlib
import json
import os
import re
import subprocess
import textwrap
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TEMP = ROOT / 'build-dev/tmp'
TEMP.mkdir(parents=True, exist_ok=True)
ENV = dict(os.environ, TMP=str(TEMP), TEMP=str(TEMP), TMPDIR=str(TEMP), PYTHONDONTWRITEBYTECODE='1')
OUT = HERE / 'diagrams'
OUT.mkdir(exist_ok=True)
revision = subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, env=ENV, text=True).strip()
source_map = {}
def source(path, needle):
    lines = (ROOT / path).read_text().splitlines()
    matches = [i + 1 for i, line in enumerate(lines) if needle in line]
    if not matches:
        raise ValueError(f'Missing source anchor: {path}: {needle}')
    line = matches[0]
    key = f'{path}:{line}'
    source_map[key] = {'path': path, 'line': line, 'anchor': needle,
        'sha256': hashlib.sha256((ROOT / path).read_bytes()).hexdigest()}
    return key

def evidence(*pairs):
    return [source(*pair) for pair in pairs]

def issue(code, kind, title, body, action, refs):
    return dict(code=code, kind=kind, title=title, body=body, action=action, refs=refs)

def node(key, label): return (key, label)
def edge(a, b, label='', kind='flow'): return (a,b,label,kind)
def group(key, title, nodes): return dict(key=key,title=title,nodes=nodes)

checks = {}
for name in ('domain_shape_check','game_dependency_check','render_dependency_check'):
    result = subprocess.run(['python3', str(ROOT/'tools'/f'{name}.py')], cwd=ROOT, env=ENV, text=True, capture_output=True)
    if result.returncode: raise RuntimeError(result.stderr + result.stdout)
    checks[name] = result.stdout
(HERE/'checks.txt').write_text('\n\n'.join(f'$ python3 tools/{k}.py\n{v}' for k,v in checks.items()))
domain = checks['domain_shape_check']
n_handlers = int(re.search(r'^(\d+) domain handlers',domain).group(1))
n_reaches = int(re.search(r'(\d+) reaches through owner_',domain).group(1))
n_refs = int(re.search(r'(\d+) xxxRef',domain).group(1))
n_callbacks = int(re.search(r'(\d+) set\*Callback',domain).group(1))

sections = []
sections.append(dict(slug='01-threading-model', short='Потоки', title='Threading Model', subtitle='Кто работает параллельно, через что передаёт результат и где кадр обязан ждать.',
    legend='Сплошные стрелки — передача работы или данных. Пунктир — ожидание или общие ресурсы. GPU — отдельная исполнительная область, не C++-поток.',
    groups=[
        group('mainlane','Главный поток · вся игровая сессия',[
            node('main','SDL / ClientLoop\nGame + Lua + подготовка сцены'),
            node('packets','Сетевые очереди\n4096 socket → 8192 game'),
            node('finalize','Готовые CPU-данные\nпроверка token → GPU upload'),
            node('record','Render: prepareRender\n[T2] futures.get → submit')]),
        group('workerlanes','Фоновые исполнители · независимые лимиты [T3]',[
            node('net','WorldSocket pump · 1\nrecv / decrypt / framing'),
            node('io','[T1] ioWorkers · 2\nFIFO задач без лимита длины'),
            node('terrain','Terrain workers · обычно 4–8\nloadQueue → ADT / WMO / M2'),
            node('ready','readyQueue / finalizingTiles\nqueueMutex + queueCV'),
            node('frame','[T2] frameWorkers · 2–16\nанимация / floor / culling / secondary'),
            node('normals','Character NormalMapBuilder · 2\n64 ожидающих · ближние первыми'),
            node('preload','World preload · до 4\nотдельные потоки → join'),
            node('other','Задачи по необходимости\nDNS/connect · Warden · patch/hash\nwatchdog · stats · update · recorder')]),
        group('external','Устройства и общие ресурсы',[
            node('assets','AssetManager\nfile mutex / DBC shared_mutex'),
            node('audio','miniaudio device callback\nмикширование PCM'),
            node('gpu','Vulkan GPU\nupload fences / frame fences')])],
    edges=[edge('main','io','JobGroup / music / spell assets'),edge('main','net','connect / stop + join','borrow'),
        edge('net','packets','callbackMutex'),edge('packets','main','opcode dispatch'),edge('main','terrain','запрос tiles'),
        edge('terrain','ready','готовые PendingTile'),edge('ready','finalize','poll + token'),edge('io','finalize','future ready'),
        edge('normals','finalize','takeFinished'),edge('main','frame','submit'),edge('frame','record','get: барьер','wait'),
        edge('finalize','gpu','асинхронные copies'),edge('record','gpu','команды кадра'),edge('gpu','record','ожидание frame slot','wait'),
        edge('terrain','assets','read / parse','borrow'),edge('io','assets','read / decode','borrow'),edge('preload','assets','warm cache','borrow'),
        edge('main','audio','управление звуками'),edge('main','normals','submit'),edge('main','other','запуск / poll')],
    issues=[
        issue('T1','Факт → риск','Все обычные I/O-задачи делят два FIFO-воркера',
            'JobGroup отправляет map/entity preparation в тот же ioWorkers, что чтение музыки и spell assets. Очередь ThreadPool не ограничена и не имеет приоритетов. Длинный backlog может задерживать появление близких объектов или запуск музыки.',
            'Измерять время ожидания в очереди, её длину и тип задачи; затем вводить приоритеты и backpressure по общему контракту.',
            evidence(('include/core/thread_pool.hpp','static ThreadPool& ioWorkers()'),('include/core/thread_pool.hpp','tasks_.emplace'),('include/core/job_group.hpp','auto future = ThreadPool::ioWorkers().submit('),('src/audio/music_manager.cpp','core::ThreadPool::ioWorkers().submit'))),
        issue('T2','Факт → риск','Параллельная работа остаётся на критическом пути кадра',
            'Главный поток ждёт M2 animation в Present и окончания secondary command buffers в Render. Это необходимые барьеры: длительность кадра зависит от самого долгого незавершённого задания, даже если остальные воркеры свободны.',
            'Профилировать очередь отдельно от выполнения и ожидания get; сохранять барьер перед чтением изменяемых spatial/animation данных.',
            evidence(('src/rendering/renderer.cpp','try { m2AnimFuture.get(); }'),('src/rendering/renderer.cpp','if (terrainFuture.valid()) lastTerrainRenderMs = terrainFuture.get();'))),
        issue('T3','Риск','Нет единого лимита фоновой загрузки CPU',
            'Frame pool, terrain pool, preload, normal maps и std::async задают параллелизм независимо. Их одновременная активность может вытеснять главный поток. Это риск oversubscription, а не доказанный замер фриза.',
            'Снять профиль холодного входа в город: runnable threads, загрузка CPU и задержки main; учитывать все активные пулы.',
            evidence(('src/rendering/terrain_manager.cpp','int computeTerrainWorkerCount()'),('include/core/thread_pool.hpp','static size_t defaultThreadCount()'),('src/core/world_loader.cpp','int numWorkers ='),('src/rendering/character_renderer.cpp','constexpr size_t kNormalMapWorkers')))
    ],
    facts=[('Главный поток','SDL, сетевой dispatch, домены игры, UI/Lua, upload/finalization и Vulkan submit/present. Отдельного постоянного render thread нет.'),
        ('Сеть','Опциональный pump; WOWEE_NET_ASYNC_PUMP=0 переносит чтение на main. ioMutex защищает socket/cipher, callbackMutex — очередь.'),
        ('Terrain','queueMutex + queueCV; readyQueue ограничивает старт новой подготовки при глубине ≥3, есть MemoryMonitor throttling. Уже выполняемые задачи могут добавить результаты.'),
        ('Frame workers','2–16 потоков, по умолчанию hardware_concurrency−1 с clamp; общий пул для анимации, коллизий, visibility и записи secondary commands.'),
        ('I/O workers','2 потока; JobGroup даёт token и lifetime fence, а не собственный пул.'),
        ('Normal maps','У CharacterRenderer собственный NormalMapBuilder: 2 потока, очередь 64, приоритет расстояния, completed results → main.'),
        ('World preload','До 4 потоков на прогрев файлов; отдельная отмена и join, не общий ioWorkers.'),
        ('Прочие роли','DNS/connect detached с shared job; Warden/patch/hash/background decode — task-scoped; watchdog и ProcessStats — отдельные потоки; recorder и встроенный asset panel — условные. Число потоков библиотек/драйвера отдельно не инвентаризировалось.')],
    good='Сетевой pump не меняет EntityManager. Данные карты помечаются token, нормальные фоновые consumer-пути опрашивают готовность; GPU upload и управление Vulkan остаются у main.'
))

sections.append(dict(slug='02-frame-lifecycle',short='Кадр',title='Frame Lifecycle',subtitle='Один неизменяемый FramePlan на кадр и десять фаз в фактическом порядке.',
    legend='Сплошная цепочка — порядок вызовов. Пунктир — отложенная доставка или влияние на последующую фазу. Present здесь означает подготовку сцены; показ swapchain происходит внутри Render.',
    groups=[group('frameorder','ClientLoop::runFrame · главный поток',[
        node('plan','FramePlan = framePlan()\nвыбирается до Input'),node('input','1 · Input\nSDL / pad → InterfaceInput'),
        node('network','2 · Network\nlogin / patches / world packets'),node('game','3 · Game\nsimulation / player / presentation requests'),
        node('events','4 · Events [F2]\nEventBus::dispatchPosted'),node('stream','5 · Streaming\nWorldLoader::update / progress'),
        node('interface','6 · Interface\nqueued game events → input → Lua/UI'),node('present','7 · Present [F1]\ncamera + renderer.update\nterrain finalize / animation → wait'),
        node('render','8 · Render\nframe-slot wait → acquire → record\nsecondary get → submit → vkQueuePresent'),
        node('transition','9 · Transitions\nstate / CharacterSession / MapScope'),node('pace','10 · Pace\nвыбранный cap / VSync'),node('next','Следующий кадр\nновый FramePlan')]),
        group('channels','Доставка и накопленная стоимость',[
            node('sync','[F2] EventBus::publish\nсинхронно в месте вызова'),node('luaqueue','Game FrameXML FIFO\nи InterfaceInput'),
            node('request','WorldTransitionQueue\nпоследние requests / leave priority'),node('budgets','[F3] Независимые бюджеты\nсеть + finalize + unload + record')])],
    edges=[edge(a,b) for a,b in zip(['plan','input','network','game','events','stream','interface','present','render','transition','pace'],['input','network','game','events','stream','interface','present','render','transition','pace','next'])]+[
        edge('network','sync','publish','borrow'),edge('game','sync','publish','borrow'),edge('network','luaqueue','уведомления','borrow'),
        edge('luaqueue','interface','drain FIFO','borrow'),edge('sync','request','только request','borrow'),edge('request','transition','применить после submit','borrow'),
        edge('budgets','present','время складывается','borrow')],
    issues=[
        issue('F1','Факт','Streaming не содержит всей работы стриминга',
            'Фаза Streaming опрашивает WorldLoader и loading scene, но terrain.update → processReadyTiles выполняется в Renderer::update, вызванном из Present. Название фазы не отражает всю её стоимость; смотреть только метрику Streaming недостаточно.',
            'При анализе фризов раскрывать Present на terrain finalization, unload, animation и wait; не переносить вызовы без проверки зависимостей.',
            evidence(('src/core/application.cpp','void Application::streamScene('),('src/core/application.cpp','void Application::present('),('src/rendering/renderer.cpp','terrainManager->update(*camera, deltaTime);'),('src/rendering/terrain_manager.cpp','processReadyTiles();'))),
        issue('F2','Факт → риск','Events — не единственное место выполнения подписчиков',
            'publish вызывает подписчиков сразу, например из Network или Game. Только post ждёт Events. FrameXML идёт отдельной FIFO в Interface; его обработчик может видеть более новое состояние игры, чем в момент публикации.',
            'Указывать канал и фазу у нового события; синхронные подписчики переходов должны только записывать запрос. Стоимость publish относится к фазе отправителя.',
            evidence(('include/core/event_bus.hpp','void publish(const Event& event)'),('include/core/event_bus.hpp','void dispatchPosted()'),('src/core/client_loop.cpp','services_.game.dispatchAddonEvents();'),('docs/principles/client-frame-phases.md','A handler may observe newer game state'))),
        issue('F3','Факт → риск','Локальные бюджеты не дают общего потолка кадра',
            'Сеть по умолчанию получает 4 мс в мире, 8 мс при входе и 12 мс при catch-up. Terrain finalize получает 2/8/16 мс, unload — ещё 8/16 мс. Это независимые лимиты; проверка между операциями не прерывает один долгий шаг. Это настройки времени, не измеренная длительность кадра.',
            'Сопоставлять суммарные CPU-фазы с целевым временем кадра и отдельно отмечать overruns одной операции и ожидания GPU.',
            evidence(('src/game/game_handler_packets.cpp','float incomingPacketBudgetMs('),('src/game/game_handler_packets.cpp','float incomingPacketCatchUpBudgetMs()'),('src/rendering/terrain_manager.cpp','const float budgetMs = taxiStreamingMode_ ? 16.0f : (nearestInView ? 8.0f : 2.0f);'),('src/rendering/terrain_manager.cpp','void TerrainManager::processPendingUnloads()')))
    ],
    facts=[('Input','beginFrame вычисляет dt; FramePlan уже выбран. SDL/pad читаются, Lua-backed input копится для Interface.'),
        ('Network','Auth/patch update, world socket transfer и ограниченный по времени opcode dispatch. Часть синхронных уведомлений возникает уже здесь.'),
        ('Game','Симуляция, reconnect, logout timeout, player/world client logic; подготовка запросов отображения.'),
        ('Events','Снимок posted queue → подписчики. Новые post из обработчика остаются до следующего dispatch.'),
        ('Streaming','WorldLoader polling и прогресс, загрузочная сцена. Нет вложенного SDL/game/render цикла.'),
        ('Interface','Вход интерфейса в мир, bindings, queued FrameXML, input, screens, ReloadUI, addon update, draw lists. При уходе — PLAYER_LEAVING_WORLD.'),
        ('Present','PlayerView и передача мира, Renderer::update, terrain finalization/unload, animation + get, наблюдение readiness загрузки.'),
        ('Render','Renderer beginFrame → frame-slot fence/timeline wait + acquire → запись проходов → secondary barriers → endFrame submit/present.'),
        ('Transitions','Запросы world entry, transfer, logout, reconnect, state. Только здесь создают/заканчивают мировые lifetimes.'),
        ('Pace','Единственное pacing место. Выбранный cap, VSync через presentation; затем Idle и отчёт frameFinished.')],
    good='LOADING_WORLD проходит тот же цикл: сеть и simulation продолжаются, интерфейс показывает прогресс, готовность проверяется после Present. Мутации lifetime применяются после submit; Lua entry защищён фазой Interface. Закрытие окна может прервать кадр после Input или Interface.'
))

sections.append(dict(slug='03-component-diagram',short='Подсистемы',title='Component Diagram',subtitle='Направления зависимостей библиотек и связи между подсистемами клиента.',
    legend='Сплошная стрелка A → B означает «A зависит от B». Пунктир внутри клиента — runtime-взаимодействие. Транзитивные и внешние библиотечные зависимости частично опущены.',
    groups=[group('client','wowee_client_objects / wowee_client · композиция',[
        node('app','Application + ClientLoop\nсоздание и wiring'),node('ui','UI + Addons / Lua\nFrameXML / screens'),
        node('presentation','PlayerView / EntitySpawner\nSpellPresentation / callbacks'),node('sound','AudioCoordinator\nminiaudio / managers')]),
        group('gamebox','wowee_game · protocol state',[
            node('gh','[C1/C2] GameHandler\nsession / queue / dispatch / facade'),node('domain','Доменные handlers\nchat / spell / inventory / quest / …'),
            node('old','[C1] 4 legacy handlers\nCombat / Movement / Entity / Warden')]),
        group('renderbox','wowee_render · сцена и GPU',[
            node('renderer','Renderer / VkContext API\nterrain / M2 / WMO / characters\nRT / water / post / overlays'),node('boundary','[C3] граница CMake\nне вся папка rendering — эта библиотека')]),
        group('libraries','Общие нижние слои',[
            node('netlib','wowee_net\nAuth / WorldSocket / HTTPS'),node('protocol','wowee_protocol\nPacket / opcodes / parsers / crypto'),
            node('pipeline','wowee_pipeline\nAssetManager / DBC / ADT / M2 / WMO'),node('base','wowee_base\nEventBus / jobs / logging / CVar / math')])],
    edges=[edge('app','ui'),edge('app','presentation'),edge('app','sound'),edge('app','gh'),edge('app','renderer'),edge('app','pipeline'),
        edge('ui','gh'),edge('ui','renderer'),edge('ui','pipeline'),edge('presentation','gh'),edge('presentation','renderer'),edge('presentation','pipeline'),
        edge('gh','domain','создаёт и делегирует','borrow'),edge('gh','old','создаёт','borrow'),edge('old','gh','GameHandler& owner_','wait'),
        edge('gh','netlib'),edge('gh','protocol'),edge('gh','pipeline'),edge('gh','base'),edge('domain','base','events / queries'),
        edge('renderer','pipeline'),edge('renderer','base'),edge('boundary','renderer','список исключений','borrow'),edge('netlib','protocol'),
        edge('netlib','base'),edge('protocol','base'),edge('pipeline','base'),edge('gh','presentation','уведомления: bus / callbacks','borrow')],
    issues=[
        issue('C1','Подтверждённый долг','Четыре домена всё ещё зависят от всего GameHandler',
            f'Статическая проверка: {n_handlers} handlers с GameHandler&, {n_reaches} обращений через owner_, {n_refs} xxxRef-accessors. Имена доменов уже выделены, но часть их зависимостей и владения состоянием остаётся неявной.',
            'Переносить state к владельцу и передавать узкие PacketSender / FrameXmlEvents / Query interfaces; не добавлять новые owner_. и Ref-пути.',
            evidence(('tools/domain_shape_check.py','def main()'),('src/game/game_handler.cpp','movementHandler_  = std::make_unique<MovementHandler>(*this);'),('include/game/game_handler.hpp','class GameHandler'))),
        issue('C2','Долг → риск','Сохраняются каналы уведомления с одним слушателем',
            f'В GameHandler осталось {n_callbacks} set*Callback setters. Они требуют ручного wiring и допускают замену предыдущего слушателя. Само число не доказывает потерю события сейчас, но показывает объём неявного контракта.',
            'При изменении существующего уведомления переходить на типизированное событие и RAII Subscription; FrameXML оставить отдельным контрактом Lua.',
            evidence(('tools/domain_shape_check.py','CALLBACK_SETTER ='),('src/core/client_loop.cpp','services_.game.setAddonEventCallback('),('include/core/event_bus.hpp','class Subscription'))),
        issue('C3','Подтверждённый долг','Граница подсистемы не совпадает с путём файла',
            'CMake исключает camera_controller, animation_controller, spell visuals и другие presentation-файлы из wowee_render. В game также остаётся 6 includes общих animation/movement constants из rendering/. Это усложняет определение владельца по каталогу.',
            'Ориентироваться на CMake targets и guard checks; общие protocol/animation IDs постепенно переносить в нейтральные headers.',
            evidence(('CMakeLists.txt','set(WOWEE_RENDER_CLIENT_SOURCES'),('tools/render_dependency_check.py','def library_sources()'),('tools/game_dependency_check.py','SHARED_CONSTANTS =')))
    ],
    facts=[('Client','Application владеет и связывает game, render, assets, UI и audio; ClientLoop управляет кадром и transitions. UI, addons, audio показаны как подсистемы, а не отдельные wowee_* targets.'),
        ('Game','Domain handlers владеют своими opcode/state; GameHandler сохраняет session/dispatch и legacy facade. Команды идут владельцу, queries — через interfaces, notifications — в bus.'),
        ('Renderer','Получает scene/presentation state, не включает game/ui/audio/network client modules. 83 source-файла в текущей проверке; compile-time границы зависят от CMake.'),
        ('Pipeline','Загрузка файлов, архивные слои, кеши и парсеры. Зависит от base, не от game/render клиента.'),
        ('Network / protocol','Сеть использует protocol; game использует net и protocol. Сетевые transport-данные не вызывают GPU напрямую.'),
        ('Headless validation','GameHarness ведёт real GameHandler без client; scene_render связывает wowee_render без game. ClientHarness ведёт тот же ClientLoop с front-end stand-ins.')],
    good='Проверки дали 0 includes presentation из game и 0 includes клиента из wowee_render. Общие константы из rendering учитываются отдельно. Это успешная статическая проверка includes, не доказательство отсутствия всех runtime-зависимостей.'
))

sections.append(dict(slug='04-data-flow',short='Данные',title='Data Flow Diagram',subtitle='Два входа — сервер и ресурсы — сходятся в presentation scene перед GPU.',
    legend='Сплошные стрелки показывают движение данных. Пунктир — уведомления, повторные запросы или подтверждение готовности. Логическая Entity и GPU instance имеют разных владельцев.',
    groups=[group('wire','Сервер → авторитетное состояние игры',[
        node('server','World server\nTCP bytes'),node('socket','WorldSocket pump\ndecrypt + framing'),
        node('sq','Socket callback FIFO\n4096 → overflow disconnect'),node('gq','[D1] Game inbound FIFO\n8192 → overflow drops packet'),
        node('dispatch','Main: budgeted dispatch\n4 / 8 / catch-up 12 мс'),node('state','Domain state / EntityManager\nлогический игровой мир'),
        node('ev','Bus notifications + FrameXML FIFO')]),
        group('resources','Ресурсы → подготовленные CPU-данные',[
            node('disk','Loose files + archive layers\nADT / M2 / WMO / BLP / DBC'),node('cache','[D2] AssetManager\nfile LRU + DBC cache / generations'),
            node('workers','Terrain / ioWorkers / normals\nread → parse → decode → composite'),
            node('prepared','[D3] CPU ready results\nPendingTile / prepared model / pixels')]),
        group('scene','Главный поток → видимый мир → GPU',[
            node('spawner','EntitySpawner / PlayerView\nGUID → model / appearance requests'),node('validate','Проверить map/request token\nотбросить устаревший result'),
            node('upload','GPU upload batch\nstaging → submit, без wait'),node('instances','Renderer instance state\nmodels + material + transform / bones'),
            node('gpu','GPU buffers / textures\nframe passes → swapchain')])],
    edges=[edge('server','socket'),edge('socket','sq','callbackMutex'),edge('sq','gq','main transfer'),edge('gq','dispatch'),edge('dispatch','state'),
        edge('state','ev','publish / queue'),edge('ev','spawner','spawn / appearance'),edge('state','spawner','queries','borrow'),
        edge('spawner','workers','запрос ресурсов'),edge('disk','cache'),edge('cache','workers','immutable/shared CPU bytes'),edge('workers','prepared'),
        edge('prepared','validate','poll готовности'),edge('validate','upload','актуальные данные'),edge('upload','instances','создать / заменить instance'),
        edge('spawner','instances','transform / pose'),edge('instances','gpu','record + submit'),edge('upload','gpu','copy commands'),edge('gpu','instances','poll upload fence','borrow')],
    issues=[
        issue('D1','Подтверждённая проблема при переполнении','Игровая очередь теряет пакеты и продолжает сессию',
            'При заполнении 8192 slots enqueueIncomingPacket удаляет самый старый пакет; enqueueIncomingPacketFront удаляет самый новый. Это общий протокольный поток, поэтому может пропасть не только промежуточное движение, но и изменение состояния. Socket queue при собственном overflow, напротив, закрывает соединение.',
            'Нужна явная recovery/resync политика. До доказанной безопасной coalescing-семантики нельзя произвольно отбрасывать пакеты состояния. Сценарий overflow требует отдельного поведенческого теста.',
            evidence(('src/game/game_handler_packets.cpp','void GameHandler::enqueueIncomingPacket('),('src/game/game_handler_packets.cpp','void GameHandler::enqueueIncomingPacketFront('),('src/network/world_socket.cpp','if (pendingPacketCallbacks_.size() > kMaxQueuedPacketCallbacks)'))),
        issue('D2','Факт → риск','Параллельные cache miss одного файла могут дублировать I/O',
            'readFileShared сначала проверяет cache, затем читает источник вне lock, затем вставляет результат. In-flight request table в этом пути нет: несколько воркеров на холодном файле могут одновременно прочитать его. Generation защищает актуальность cache, но не объединяет запросы.',
            'Сначала измерить дубликаты cold reads и peak decoded bytes; если существенно — объединять одновременные запросы по нормализованному ключу и generation.',
            evidence(('src/pipeline/asset_manager.cpp','AssetManager::FileBytes AssetManager::readFileShared('),('src/pipeline/asset_manager.cpp','std::vector<uint8_t> data = readFromSource(normalized);'))),
        issue('D3','Риск','Порог readyQueue измеряет tiles, а не память всех результатов',
            'Порог 3 проверяется перед началом terrain preparation; уже работающие workers могут положить дополнительные tiles. Разные tiles содержат разное количество decoded pixels и моделей. MemoryMonitor уже замедляет загрузку, но этот порог не является жёстким byte cap всего pipeline.',
            'Наблюдать queued + preparing + ready + finalizing CPU bytes и staging bytes; оценивать backpressure по общему объёму, сохраняя прогресс хотя бы одного tile.',
            evidence(('include/rendering/terrain_manager.hpp','static constexpr size_t maxReadyQueueSize_ = 3;'),('src/rendering/terrain_manager.cpp','if (readyQueue.size() >= maxReadyQueueSize_)'),('src/rendering/terrain_manager.cpp','readyQueue.push(pending);')))
    ],
    facts=[('Сетевой handoff','Фоновая очередь socket → main → очередь game. Первая защищена mutex; вторая — main-thread-only и бюджетирована по времени.'),
        ('Домены','Packet parser + opcode handler изменяют state. EntityController владеет entities; отображение получает события и запрашивает state.'),
        ('Ресурсный handoff','Workers готовят CPU-результаты. Main опрашивает future/ready queue и проверяет token карты и запроса перед использованием.'),
        ('Актуальность','JobGroup::replace(channel, GUID) отменяет предыдущий запрос; map cancellation отсекает результаты старой карты, даже если GUID переиспользован.'),
        ('GPU handoff','Upload batch записывает copies и submit с fence. Fence опрашивается в последующих кадрах. Наличие готовых CPU bytes не равнозначно завершённому upload.'),
        ('UI / audio branches','FrameXML FIFO читается в Interface; audio callbacks и presentation-подписчики выбирают звуки на основании игровых событий. Эти ветви не превращают game в renderer.')],
    good='Есть parse/read limits, bounded packet queues, packet-lag catch-up, terrain memory throttling, request tokens и асинхронные upload batches. Выявленные проблемы относятся к overflow policy и сквозному учёту нагрузки, а не к отсутствию этих механизмов.'
))

sections.append(dict(slug='05-ownership-lifetime',short='Владение',title='Ownership / Lifetime Model',subtitle='Владение, заимствованные ссылки и границы освобождения CPU/GPU объектов.',
    legend='Сплошная стрелка owner → child — владение (unique_ptr или value). Пунктир — заимствованная ссылка. Пунктир с подписью retire — отложенное освобождение GPU, не владение.',
    groups=[group('process','Жизнь процесса · Application',[
        node('app','Application\n[O2] явный shutdown order'),node('bus','EventBus\nживёт дольше subscribers'),node('window','Window → VkContext\ndevice / swapchain / fences'),
        node('renderer','Renderer\nmodel / texture caches + render layers'),node('assets','AssetManager\nshared immutable file / DBC cache'),
        node('gh','GameHandler\nconnection + domains'),node('entities','Handlers / EntityController\ncharacter state / entities'),node('front','PlayerView / EntitySpawner\nWorldLoader / UI / audio'),
        node('loop','ClientLoop\nworld transitions')]),
        group('character','Жизнь персонажа · CharacterSession',[
            node('session','CharacterSession\nlogin → logout / exit'),node('character','InterfaceState + PlayerSync\nSpellPresentation + subscriptions'),
            node('lua','AddonManager / Lua state\nsave variables → close')]),
        group('map','Жизнь карты · MapScope',[
            node('map','MapScope\nentry → supersede / leave'),node('jobs','[O1] JobGroup\ntokens + tracked leases'),node('spawnstate','EntitySpawner::SpawnState\nper-GUID requests / pending results'),
            node('load','WorldLoader::Session\nprepared map / future'),node('renderSession','MapRenderSession\ninstance sessions + TerrainManager')]),
        group('special','За границей обычного owner lifetime',[
            node('retire','GPU retirement\nVkContext deferredCleanup[slot]'),node('connector','[O3] AsyncConnect detached worker\nshared Job переживает cancel')])],
    edges=[edge('app',k) for k in ['bus','window','renderer','assets','gh','front','loop']]+[
        edge('gh','entities'),edge('loop','session'),edge('session','character'),edge('session','lua'),edge('session','map'),
        edge('map','jobs'),edge('map','spawnstate'),edge('map','load'),edge('map','renderSession'),
        edge('renderer','window','borrow VkContext','borrow'),edge('front','spawnstate','bindMap borrowed','borrow'),
        edge('front','load','WorldLoader.load_ borrowed','borrow'),edge('renderSession','renderer','instance layers borrowed','borrow'),
        edge('jobs','assets','задачи читают assets','borrow'),edge('renderSession','retire','retire после submit','borrow'),
        edge('retire','window','slot fence → cleanup','borrow'),edge('gh','connector','connect job / socket handoff','borrow')],
    issues=[
        issue('O1','Факт → риск','Отмена задач не делает teardown мгновенным',
            'MapScope отменяет jobs, уничтожает load session, останавливает terrain и вызывает wait. JobGroup учитывает lease ещё до постановки в общую I/O FIFO. Поэтому отменённая, но ещё не запущенная задача может ждать предыдущие чужие задачи; уже идущий read/parse не прерывается токеном.',
            'Измерять Transitions и возраст отменённых jobs; при требовании ограниченной задержки нужен scheduler с удалением queued cancelled work или эквивалентный контракт fence.',
            evidence(('src/core/map_scope.cpp','MapScope::~MapScope()'),('include/core/job_group.hpp','auto lease = track(t);'),('src/core/world_loader.cpp','WorldLoader::Session::~Session()'),('include/core/thread_pool.hpp','tasks_.emplace'))),
        issue('O2','Долг → риск','Безопасность borrowed ссылок зависит от ручного teardown order',
            'Renderer заимствует VkContext у Window; spawner — SpawnState карты, WorldLoader — Session load. Application::shutdown явно уничтожает клиентские lifetime до assets/device/window. CharacterSession также вручную сбрасывает состояние process-wide presenters. Эти связи не выражены одним owner tree.',
            'Новые подписки хранить RAII-членами; для каждого нового borrowed указателя указать lifetime. Проверять logout, same-map reconnect, superseded load и exit, сохраняя порядок stop/join → release.',
            evidence(('include/core/window.hpp','std::unique_ptr<rendering::VkContext> vkContext;'),('include/rendering/renderer.hpp','VkContext* vkCtx = nullptr;'),('src/core/application.cpp','void Application::shutdown()'),('src/core/character_session.cpp','CharacterSession::~CharacterSession()'))),
        issue('O3','Факт → риск','AsyncConnect.cancel оставляет DNS/connect работать до возврата',
            'Detached worker удерживает shared Job, а cancel ставит abandoned. Это сохраняет память и закрывает поздний socket, но не прерывает connectBlocking / DNS. Повторные попытки могут временно перекрываться по потокам; реальная задержка здесь не измерялась.',
            'Если нужна строгая отмена и ограничение повторов — ограничивать число connect jobs и использовать отменяемый transport/resolver. Не заменять shared Job захватом сырого owner.',
            evidence(('include/network/async_connect.hpp','std::thread([job, host, port, timeoutSeconds]()'),('include/network/async_connect.hpp','void cancel()')))
    ],
    facts=[('Создание','Application строит долгоживущие сервисы. ClientLoop в Transitions создаёт CharacterSession; CharacterSession::enterMap заменяет только MapScope.'),
        ('Смена карты','Сначала уничтожается прежний MapScope, даже при повторном входе в тот же map ID. InterfaceState, Lua и остальная сессия персонажа сохраняются.'),
        ('Reconnect','Сцена может стоять во время восстановления соединения. При успешном same-map reconnect MapScope передаётся releaseMap/adoptMap новой CharacterSession; новый Lua interface слышит login заново.'),
        ('Деструктор карты','cancel tokens → unbind spawner → load.reset / terrain.stopWorkers → jobs.wait → detach player/transport references → render session release. Process-wide model/texture caches сохраняются.'),
        ('Деструктор персонажа','MapScope уходит первым; затем AddonManager сохраняет переменные и закрывает Lua внутри Interface scope; затем остальное per-character state и subscriptions.'),
        ('GPU lifetime','MapRenderSession освобождает logical instance contents; Vulkan destruction откладывается за slot fences. В обычном map teardown нет vkDeviceWaitIdle; shutdown приложения использует device idle перед освобождением device resources.'),
        ('Типы владения','unique_ptr/value: единственный владелец. shared_ptr<const bytes/table>: разделяемые CPU-данные. Raw pointers/references: borrow и обязательный порядок уничтожения. Token: актуальность/отмена, не владение Renderer или AssetManager.')],
    good='MapScope и CharacterSession уже объединяют основные lifetime. JobGroup fence удерживает CPU-задачи до их завершения, RAII Subscription отключает слушателя, deferred GPU cleanup защищает ещё используемые ресурсы. Кеш между картами сам по себе не является утечкой.'
))

# Keep the main-thread producer and consumer portions separate in the drawing:
# a worker result returns to the same main thread, not to an extra render thread.
thread = sections[0]
thread_nodes = {key: label for g in thread['groups'] for key,label in g['nodes']}
thread['groups'] = [
    group('incoming','Сеть → главный поток',[node(k,thread_nodes[k]) for k in ('net','packets','main')]),
    group('workers','Фоновые исполнители · независимые лимиты [T3]',[node(k,thread_nodes[k]) for k in ('io','frame','terrain','normals','preload','other')]),
    group('returnmain','Результаты → тот же главный поток',[node(k,thread_nodes[k]) for k in ('ready','finalize','record')]),
    group('devices','Общие ресурсы и устройства',[node(k,thread_nodes[k]) for k in ('assets','audio','gpu')]),
]
thread['ranks'] = [('io','frame'),('terrain','normals'),('preload','other')]
thread['invisible'] = [('io','terrain'),('terrain','preload')]
thread['edges'] = [e for e in thread['edges'] if not (e[0]=='main' and e[1]=='net')]
thread['edges'] = [(a,b,label,'borrow' if a in ('preload',) or b in ('other','audio') else kind) for a,b,label,kind in thread['edges']]
owner = sections[4]
owner['ranks'] = [('bus','window'),('renderer','assets'),('gh','front'),('jobs','spawnstate'),('load','renderSession')]
owner['invisible'] = [('bus','renderer'),('renderer','gh'),('gh','loop'),('jobs','load')]
component = sections[2]
component['edges'] = [(a,b,label,'flow' if a=='gh' and b in ('domain','old') else kind) for a,b,label,kind in component['edges']]
component['ranks'] = [('ui','presentation','sound'),('gh','renderer'),('domain','old'),('pipeline','netlib')]

CSS = '''
:root {color-scheme:light dark;--bg:light-dark(#f4f6fa,#11151e);--fg:light-dark(#172334,#e5edf7);--panel:light-dark(#fff,#1a2230);--line:light-dark(#d9e1ec,#344153);--muted:light-dark(#53647a,#afbdd0);--blue:light-dark(#175a9c,#8fc7ff);--red:light-dark(#a03932,#ffaca6)}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.6 system-ui,-apple-system,'Segoe UI',sans-serif}a{color:var(--blue);text-underline-offset:3px}header,main,footer{max-width:1800px;margin:auto;padding:24px 36px}header{padding-bottom:12px}h1{font-size:32px;line-height:1.15;margin:10px 0 16px;font-weight:650}h2{font-size:26px;margin:0 0 6px;line-height:1.3}h3{font-size:17px;line-height:1.4;margin:8px 0}p{margin:8px 0 14px}.meta,.legend,small{color:var(--muted)}.kicker{letter-spacing:.12em;text-transform:uppercase;font-size:12px;color:var(--blue)}nav{display:flex;gap:8px;flex-wrap:wrap;margin:20px 0}nav a{padding:8px 14px;border:1px solid var(--line);border-radius:6px;text-decoration:none;color:var(--fg)}nav a[aria-current]{background:var(--fg);color:var(--bg)}.page{margin:24px 0 64px;scroll-margin-top:24px}.intro{max-width:1100px}.layout{display:grid;grid-template-columns:minmax(0,1fr) 370px;gap:28px;align-items:start;margin:20px 0}.diagram{background:#fff;border-radius:8px;padding:14px;overflow:auto}.diagram svg{display:block;width:100%;height:auto;min-width:640px}.figuretools{display:flex;gap:8px;margin-bottom:8px}.figuretools button{font-size:13px;background:var(--panel);color:var(--fg);border:1px solid var(--line);border-radius:4px}.diagram{max-height:1100px}.legend{font-size:13px;margin:12px 0}.issue{border-top:2px solid var(--line);padding:14px 0 18px}.issue:first-child{border-color:var(--red)}.badge{font-size:12px;letter-spacing:.02em;color:var(--red)}.issue p{font-size:14px}.action{border-left:2px solid var(--line);padding-left:12px}.sources{font-size:12px;overflow-wrap:anywhere}.sources a{display:block;margin:4px 0}.good{background:var(--panel);padding:18px 22px;border-left:3px solid var(--blue);margin:24px 0}.downloads{display:flex;flex-wrap:wrap;gap:14px;font-size:13px;margin:14px 0}.details{background:var(--panel);padding:16px 22px;margin:20px 0;border:1px solid var(--line);border-radius:6px}summary{cursor:pointer;font-weight:600}table{border-collapse:collapse;width:100%;margin-top:15px;font-size:14px}th,td{padding:12px 10px;vertical-align:top;border-top:1px solid var(--line);text-align:left}th{width:190px;font-weight:600}.findings{display:grid;grid-template-columns:repeat(3,1fr);gap:22px;margin-top:24px}.findings a{font-weight:600}.findings p{font-size:14px}.toolbar{margin-top:12px}button{font:inherit;padding:6px 12px;cursor:pointer}footer{border-top:1px solid var(--line);font-size:13px;color:var(--muted)}@media(max-width:1100px){.layout{grid-template-columns:1fr}.notes{display:grid;grid-template-columns:repeat(3,1fr);gap:20px}.diagram svg{max-height:none}.findings{grid-template-columns:1fr}}@media(max-width:720px){header,main,footer{padding:20px 16px}.notes{display:block}h1{font-size:27px}.diagram{padding:8px}th{width:120px}}@media print{.diagram{max-height:none;overflow:visible}body{background:white;color:#172334;font-size:11pt}nav,.toolbar,.downloads,.figuretools{display:none}header,main,footer{padding:12px}.page{break-before:page;margin-bottom:0}.layout{grid-template-columns:minmax(0,1fr) 300px;gap:18px}.diagram svg{min-width:0}.issue p{font-size:10pt}.sources{font-size:9pt}.notes{display:block}details{display:none}.good{background:transparent}a{color:inherit}.findings{display:block}}
'''

def dot_for(s):
    lines = ['digraph atlas {', 'graph [rankdir=TB, newrank=true, bgcolor="white", pad="0.2", nodesep="0.28", ranksep="0.4", compound=true, fontname="DejaVu Sans", fontsize=15];',
        'node [shape=box, style="rounded,filled", fillcolor="#f1f5fa", color="#c4d1e2", fontcolor="#142b45", fontname="DejaVu Sans", fontsize=13, margin="0.18,0.13"];',
        'edge [color="#687f98", fontcolor="#38546f", fontname="DejaVu Sans", fontsize=10, arrowsize=0.65];']
    q = lambda x: json.dumps(x,ensure_ascii=False)
    for g in s['groups']:
        lines += [f'subgraph cluster_{g["key"]} {{',f'label={q(g["title"])}; style="rounded"; color="#c4d1e2"; fontcolor="#38546f";']
        for key,label in g['nodes']:
            attr = ',fillcolor="#fff1e9",color="#d6a68b"' if re.search(r'\[[A-Z]\d',label) else ''
            lines.append(f'{key} [label={q(label)}{attr}];')
        lines.append('}')
    for a,b,label,kind in s['edges']:
        attrs = [f'label={q(label)}'] if label else []
        if kind in ('wait','borrow'):
            attrs.append('style=dashed,constraint=false')
        if kind=='wait': attrs += ['color="#ae5b32"','fontcolor="#8a4324"']
        lines.append(f'{a} -> {b} [{",".join(attrs)}];')
    for rank in s.get('ranks',[]):
        lines.append('{rank=same; '+ '; '.join(rank)+';}')
    for a,b in s.get('invisible',[]):
        lines.append(f'{a} -> {b} [style=invis,weight=5];')
    return '\n'.join(lines+['}'])

def mermaid_for(s):
    out = ['flowchart TB']
    for g in s['groups']:
        out.append(f'  subgraph sg_{g["key"]}["{g["title"]}"]')
        for key,label in g['nodes']:
            out.append(f'    {key}["{h(label).replace(chr(10),"<br/>")}"]')
        out.append('  end')
    for a,b,label,kind in s['edges']:
        arrow = '-.->' if kind in ('borrow','wait') else '-->'
        lab = '|"'+h(label)+'"|' if label else ''
        out.append(f'  {a} {arrow}{lab} {b}')
    return '\n'.join(out)+'\n'

def source_html(refs):
    result=[]
    for key in refs:
        ref=source_map[key]
        # Relative repository links work in ordinary local browsers. The line is
        # also visible because browsers do not implement editor line fragments.
        url='../../'+ref['path']+'#L'+str(ref['line'])
        result.append(f'<a href="{h(url)}">{h(key)}</a>')
    return '<div class="sources">'+''.join(result)+'</div>'

def article(s):
    svg = (OUT/(s['slug']+'.svg')).read_text()
    svg = svg[svg.index('<svg'):]
    svg=re.sub(r'id="([^"]+)"', lambda m: 'id="'+s['slug']+'-'+m.group(1)+'"', svg)
    svg=svg.replace('<svg ',f'<svg role="img" aria-label="{h(s["title"])}: {h(s["subtitle"])}" ',1)
    notes=[]
    for i in s['issues']:
        notes.append(f'<section class="issue" id="{i["code"]}"><span class="badge">{i["code"]} · {h(i["kind"])}</span><h3>{h(i["title"])}</h3><p>{h(i["body"])}</p><p class="action">{h(i["action"])}</p>{source_html(i["refs"])}</section>')
    rows=''.join(f'<tr><th scope="row">{h(k)}</th><td>{h(v)}</td></tr>' for k,v in s['facts'])
    downloads=' · '.join(f'<a href="diagrams/{s["slug"]}.{ext}">{ext.upper()}</a>' for ext in ('svg','png','mmd','dot'))
    return f'<article class="page" id="{s["slug"]}"><h2>{h(s["title"])}</h2><p class="meta">{h(s["subtitle"])}</p><div class="downloads"><a href="{s["slug"]}.html">Отдельный артефакт</a>{downloads}</div><div class="layout"><div><div class="figuretools"><button type="button" data-zoom="in">Увеличить</button><button type="button" data-zoom="out">Уменьшить</button><button type="button" data-zoom="fit">Вписать</button></div><div class="diagram">{svg}</div><p class="legend">{h(s["legend"])}</p></div><aside class="notes" aria-label="Проблемы рядом с диаграммой">{"".join(notes)}</aside></div><p class="good"><strong>Что уже защищено.</strong> {h(s["good"])}</p><details class="details"><summary>Обязанности, границы и точный порядок</summary><table>{rows}</table></details></article>'

def wrap(title, content, selected=None):
    nav='<a href="index.html"'+(' aria-current="page"' if selected is None else '')+'>Обзор</a>'
    nav+=''.join(f'<a href="{s["slug"]}.html"'+(' aria-current="page"' if selected==s['slug'] else '')+'>'+h(s['short'])+'</a>' for s in sections)
    return f'''<!doctype html>
<html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><meta name="color-scheme" content="light dark"><title>{h(title)} · WoWee</title><style>{CSS}</style></head>
<body><header><div class="kicker">WoWee / архитектура по исходному коду</div><h1>{h(title)}</h1><p class="meta">19 октября 2026 · HEAD {revision} + рабочее дерево · статический разбор</p><nav aria-label="Диаграммы">{nav}</nav></header><main>{content}</main><footer>Факт — наблюдаемое поведение кода. Риск — возможное следствие без runtime-подтверждения. Долг — текущая архитектурная связанность. Числа бюджетов — настройки по умолчанию, а не результаты профилирования. Исходники и строки указаны рядом с каждым замечанием. <a href="README.md">Markdown</a> · <a href="sources.json">Снимок источников</a> · <a href="checks.txt">Статические проверки</a></footer><script>
document.addEventListener('click', function(event) {{
 const button=event.target.closest('button[data-zoom]'); if(!button)return;
 const svg=button.closest('article').querySelector('.diagram svg');
 if(button.dataset.zoom==='fit'){{svg.style.width='100%';return;}}
 const current=svg.getBoundingClientRect().width;
 svg.style.width=Math.max(640, Math.min(6000,current*(button.dataset.zoom==='in'?1.25:0.8)))+'px';
}});
</script></body></html>'''

for s in sections:
    dot=dot_for(s)
    dotpath=OUT/(s['slug']+'.dot')
    dotpath.write_text(dot)
    (OUT/(s['slug']+'.mmd')).write_text(mermaid_for(s))
    for ext in ('svg','png'):
        result=subprocess.run(['dot',f'-T{ext}',str(dotpath),'-o',str(OUT/(s['slug']+'.'+ext))],env=ENV,capture_output=True,text=True)
        if result.returncode or result.stderr.strip():
            raise RuntimeError(result.stderr)
    ET.parse(OUT/(s['slug']+'.svg'))
    (HERE/(s['slug']+'.html')).write_text(wrap(s['title'],article(s),s['slug']))

intro='''<div class="intro"><p>Пять представлений текущей архитектуры. Коды T/F/C/D/O на узлах связывают диаграмму с пояснениями справа. Для каждого замечания указаны наблюдение, возможное последствие, следующий шаг и место в исходниках.</p><p>Это разбор реализации: подтверждённые ограничения отделены от рисков. Фризы, утечки и гонки не объявляются воспроизведёнными без отдельного запуска. Диаграммы обзорные; все методы и все потоки сторонних библиотек не перечислены.</p></div><div class="findings"><div><a href="#D1">D1 · Потеря сетевого состояния</a><p>При overflow игровой очереди удаляется пакет, сессия продолжается. Нужна явная recovery-политика.</p></div><div><a href="#F1">F1 · Скрытая стоимость Present</a><p>Часть terrain streaming и ожидание анимации происходят после фазы Streaming.</p></div><div><a href="#O1">O1 · Отмена с ожиданием</a><p>Transitions может ждать cancelled jobs, стоящие за другой работой общего I/O-пула.</p></div></div>'''
(HERE/'index.html').write_text(wrap('Пять взглядов на движок',intro+''.join(article(s) for s in sections)))

md=['# WoWee — архитектурный атлас',f'\nСнимок: 2026-10-19, HEAD `{revision}` + рабочее дерево. Все выводы основаны на статическом чтении кода; runtime-профиль не снимался.',
    '\n[Открыть общий обзор](index.html). Пять отдельных HTML-артефактов работают без сети. В каждом диаграмма находится рядом с проблемами и ссылками на код. SVG/PNG предназначены для просмотра и экспорта, Mermaid/DOT — для редактирования.',
    '\nОбозначения: **Факт** — проверено в коде; **Риск** — возможное следствие, требующее воспроизведения/замера; **Долг** — текущая архитектурная связанность. Это описание реализации, не проект исправлений. Номера строк относятся к снимку; хеши исходников сохранены в `sources.json`.',
    '\nСуществовавшие локальные изменения server_extension и warden не изменялись. Результаты проверок относятся к прочитанному рабочему дереву. `docs/threading.md` использовался только как ориентир: его описания detached normal-map tasks и самостоятельных loading frames расходятся с текущей реализацией.',
    '\n## Артефакты\n']
for s in sections:
    md.append(f'- [{s["title"]}]({s["slug"]}.html) · [SVG](diagrams/{s["slug"]}.svg) · [PNG](diagrams/{s["slug"]}.png) · [Mermaid](diagrams/{s["slug"]}.mmd)')
for s in sections:
    md += [f'\n## {s["title"]}', '\n'+s['subtitle'],'\n```mermaid\n'+mermaid_for(s)+'```','\n'+s['legend'],'\n### Что подсвечивает\n']
    for i in s['issues']:
        md += [f'**{i["code"]} · {i["kind"]} · {i["title"]}**\n',i['body']+'\n','Следующий шаг: '+i['action']+'\n']
        # Relative source links resolve from this directory; absolute links are
        # also supplied for Codex readers, since that UI supports line targets.
        for key in i['refs']:
            r=source_map[key]
            md.append(f'- [{key}]({ROOT/r["path"]}:{r["line"]})')
        md.append('')
    md += ['\n### Обязанности и границы\n','| Область | Поведение |','|---|---|']
    md += [f'| {k} | {v} |' for k,v in s['facts']]
    md += ['\n**Существующая защита:** '+s['good']]
md += ['\n## Проверки и воспроизведение\n','`checks.txt` содержит вывод `domain_shape_check.py`, `game_dependency_check.py`, `render_dependency_check.py`. Они проверяют связанность/includes, а не поведение рендера или сети. Клиент не собирался и не перезапускался: изменения только документальные.',
    '\nГенерация: `python3 docs/architecture-atlas/generate.py` (Python 3 + Graphviz `dot`). Генератор пишет только в эту папку; TMP/TEMP/TMPDIR дочерних процессов направлены в `build-dev/tmp` на D:. PNG и SVG строятся из DOT; Mermaid передаёт тот же набор узлов и связей, его layout может отличаться.',
    '\nHTML не использует CDN, шрифты из сети или внешние скрипты. SVG/PNG содержат саму схему; пояснения рядом и ссылки доступны в HTML и Markdown. Для печати всего обзора можно использовать печать HTML в PDF.']
(HERE/'README.md').write_text('\n'.join(md)+'\n')
(HERE/'sources.json').write_text(json.dumps({'snapshot_date':'2026-10-19','head':revision,'scope':'working tree, static review','sources':source_map},ensure_ascii=False,indent=2)+'\n')
(HERE/'atlas.json').write_text(json.dumps(sections,ensure_ascii=False,indent=2)+'\n')
print(f'Built {len(sections)} diagrams, 6 offline HTML documents, Mermaid/DOT/SVG/PNG and source manifest ({len(source_map)} anchors).')
