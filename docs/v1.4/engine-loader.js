var activeReplayEngine = null;
var replayEngineReady;
var replayLoadQueue = Promise.resolve();
var remasteredTerrainPromise;
var remasteredFactoryPromise;

function replay_engine_format(bytes) {
    if (bytes.length < 30) throw new Error("录像文件过短");
    var data = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    var magic = new DataView(data.buffer, data.byteOffset).getUint32(12, true);
    if (magic !== 0x53526572 && magic !== 0x53526573) throw new Error("无法识别为星际争霸录像");
    return magic === 0x53526573 || (data[28] === 0x78 && (data[28] * 256 + data[29]) % 31 === 0)
        ? "remastered" : "classic";
}

function initialize_replay_engine(format, factory) {
    var canvas = document.getElementById("canvas");
    return new Promise(function(resolve, reject) {
        Module = factory({
            canvas: canvas,
            print: console.log,
            printErr: console.error,
            setWindowTitle: function() { document.title = window.OPENBW_WINDOW_TITLE || "STL 星际争霸录像播放器"; },
            locateFile: function(name) {
                return "v1.4/" + (format === "remastered" ? "remastered/" : "") + name + "?ver=1.5.24";
            },
            onReplayOperationWarning: show_replay_operation_warning,
            onAbort: function(reason) { reject(new Error(String(reason))); },
            onRuntimeInitialized: function() {
                Module = this;
                Module.replayFormat = format;
                Object.keys(Module).forEach(function(name) {
                    if (name[0] === "_" && typeof Module[name] === "function") window[name] = Module[name];
                });
                window.UTF8ToString = Module.UTF8ToString;
                activeReplayEngine = format;
                resolve();
            }
        });
    });
}

function get_remastered_factory() {
    if (!remasteredFactoryPromise) {
        remasteredFactoryPromise = new Promise(function(resolve, reject) {
            var script = document.createElement("script");
            script.src = "v1.4/remastered/openbw.js?ver=1.5.24";
            script.onload = function() { resolve(createRemasteredReplayEngine); };
            script.onerror = function() { reject(new Error("无法加载重制版录像引擎")); };
            document.head.appendChild(script);
        }).catch(function(error) { remasteredFactoryPromise = null; throw error; });
    }
    return remasteredFactoryPromise;
}

async function prepare_replay_engine(bytes) {
    await replayEngineReady;
    var format = replay_engine_format(bytes);
    if (format === activeReplayEngine) return;
    var factory = createClassicReplayEngine;
    if (format === "remastered") {
        factory = await get_remastered_factory();
        if (!files[3] && resourceMode !== "online") {
            throw new Error("缺少重制版录像用的 terrain.mpq。请刷新页面并在确认弹窗中选择在线加载，或连同本地游戏资源一起导入。");
        }
        if (!files[3] && !remasteredTerrainPromise) {
            remasteredTerrainPromise = fetch(C_MPQ_BASE_URL + "/remastered/terrain.mpq")
                .then(function(response) {
                    if (!response.ok) throw new Error("无法加载重制版地形资源。HTTP " + response.status);
                    return response.blob();
                }).catch(function(error) { remasteredTerrainPromise = null; throw error; });
        }
        if (!files[3]) files[3] = await remasteredTerrainPromise;
    }
    var buffers = await Promise.all(files.slice(0, format === "remastered" ? 4 : 3)
        .map(function(file) { return file.arrayBuffer(); }));
    if (main_has_been_called) {
        Module.set_volume(0);
        Module.disposeReplayEngine();
    }
    main_has_been_called = false;
    viewerMainLoopPausedForIdle = false;
    js_read_buffers = buffers.map(function(buffer) { return new Int8Array(buffer); });
    replayEngineReady = initialize_replay_engine(format, factory);
    await replayEngineReady;
}

function load_replay_bytes(bytes) {
    replayLoadQueue = replayLoadQueue.then(async function() {
        await prepare_replay_engine(bytes);
        var buffer = allocate_replay_buffer(bytes);
        try { start_replay(buffer, bytes.length); }
        finally { _free(buffer); }
    }).catch(function(error) {
        handle_replay_error(error.message || String(error));
    });
    return replayLoadQueue;
}

function show_replay_operation_warning(frame) {
    var dialog = document.getElementById("replay-operation-warning");
    if (dialog.open) return;
    document.getElementById("replay-operation-warning-time").textContent =
        "异常时间 " + format_replay_time(frame);
    dialog.returnValue = "exit";
    dialog.showModal();
}

document.getElementById("replay-operation-warning").addEventListener("close", function() {
    if (!main_has_been_called) return;
    if (this.returnValue === "continue") {
        _replay_set_value(1, 0);
    } else {
        handle_replay_error("");
    }
});

function handle_replay_error(message) {
    if (main_has_been_called) {
        Module.set_volume(0);
        Module.disposeReplayEngine();
    }
    main_has_been_called = false;
    activeReplayEngine = null;
    replayEngineReady = Promise.resolve();
    reset_playback_state_monitor();
    stop_music_playback();
    reset_pregame_dropzone();
    reveal_pregame_homepage();
    document.body.classList.add("pregame-active");
    document.getElementById("pregame-overlay").style.display = "grid";
    var dropzone = document.querySelector(".pregame-dropzone");
    var previous = dropzone.querySelector(".replay-load-error");
    if (previous) previous.remove();
    if (embeddedReplayConfig.enabled) abort_embedded_replay_load();
    if (!message) return;
    var error = document.createElement("div");
    error.className = "replay-load-error";
    error.setAttribute("role", "alert");
    if (message === "ComputerAI" || message === "AIPatrol") message = "暂不支持内置电脑 AI。";
    if (message === "disable thingy unit") message = "该自定义地图使用了暂不支持的触发器。";
    var unsupportedTrigger = /^unknown trigger (action|condition) (\d+)$/i.exec(message);
    if (unsupportedTrigger) {
        var triggerPart = unsupportedTrigger[1].toLowerCase() === "action" ? "动作" : "条件";
        message = "地图使用了当前引擎不支持的触发" + triggerPart + "（编号 " + unsupportedTrigger[2] + "）。";
    }
    error.textContent = "无法播放录像。" + message;
    dropzone.prepend(error);
}

replayEngineReady = initialize_replay_engine("classic", createClassicReplayEngine);
