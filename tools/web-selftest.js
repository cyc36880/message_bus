/**
 * web-selftest.js —— 用 Windows Script Host 跑 message-bus-designer.html 里的纯逻辑自检。
 *
 * 用法（仓库根目录或 tools/ 下都行）：
 *     cscript //nologo tools\web-selftest.js
 *
 * 为什么要有这个脚本：
 *   网页里那套「主题校验 + MQTT 通配符匹配 + 来源过滤 + NOLOCAL」的规则，
 *   必须和固件（src/mb_topic.c、src/mb_dispatch.c）**逐条一致**，否则设计器
 *   判「合法」的东西烧进 MCU 会返回 MB_ERR_INVALID_ARG。
 *
 *   所以这里不是另抄一份逻辑来测，而是从 HTML 里把 PURE-LOGIC 标记之间的
 *   **原始源码**抠出来，用 tests/test_topic.c 里同一张用例表直接跑。
 *   网页改坏了规则，这个脚本就会红。
 *
 * 输出一律英文：Windows 控制台默认编码不是 UTF-8，中文会显示成乱码。
 */
(function () {
    var fso = new ActiveXObject("Scripting.FileSystemObject");
    var htmlPath = fso.BuildPath(fso.GetParentFolderName(WScript.ScriptFullName),
                                 "message-bus-designer.html");

    if (!fso.FileExists(htmlPath)) {
        WScript.Echo("FAIL: cannot find " + htmlPath);
        WScript.Quit(2);
    }

    /* HTML 是 UTF-8，FSO 读不了，得借 ADODB.Stream 指定字符集。 */
    var src;
    try {
        var stream = new ActiveXObject("ADODB.Stream");
        stream.Type = 2;              /* adTypeText */
        stream.Charset = "utf-8";
        stream.Open();
        stream.LoadFromFile(htmlPath);
        src = stream.ReadText();
        stream.Close();
    } catch (ex) {
        WScript.Echo("FAIL: cannot read the HTML: " + ex.message);
        WScript.Quit(2);
    }

    var BEGIN = "===== BEGIN PURE-LOGIC =====";
    var END = "===== END PURE-LOGIC =====";
    var b = src.indexOf(BEGIN);
    var e = src.indexOf(END);
    if (b < 0 || e < 0 || e <= b) {
        WScript.Echo("FAIL: PURE-LOGIC markers not found (was the HTML restructured?)");
        WScript.Quit(2);
    }
    var code = src.substring(src.indexOf("\n", b) + 1, src.lastIndexOf("\n", e));

    /* 这段区域在写法上已经避开 ES6，只剩 let / const 两种语法需要降级。
       （区域内唯一出现 "let"/"const" 字样的地方是注释，被一起替换也无害。） */
    code = code.replace(/\b(let|const)\b/g, "var");

    /* JScript 只有 indexOf，补一个 includes（区域内已改回 indexOf，这里是兜底）。 */
    if (!String.prototype.includes) {
        String.prototype.includes = function (needle) { return this.indexOf(needle) !== -1; };
    }
    if (!Array.prototype.indexOf) {
        Array.prototype.indexOf = function (item) {
            for (var i = 0; i < this.length; ++i) { if (this[i] === item) { return i; } }
            return -1;
        };
    }

    /* 不能用 eval 的返回值：JScript 里 evaluated 程序若以声明开头，eval 返回 undefined。
       改成让被求值的代码往外层作用域的变量赋值，这样最稳。 */
    var selfTest = null;
    try {
        eval(code + "\nselfTest = runSelfTest;\n");
    } catch (ex) {
        WScript.Echo("FAIL: cannot evaluate the extracted code: " + ex.message);
        WScript.Quit(2);
    }
    if (typeof selfTest !== "function") {
        WScript.Echo("FAIL: runSelfTest() was not found in the extracted region "
                     + "(got " + (typeof selfTest) + ")");
        WScript.Quit(2);
    }

    var fails = selfTest();

    if (fails.length === 0) {
        WScript.Echo("message-bus-designer: " + selfTest.checks +
                     " checks, 0 failed -- topic rules agree with the C implementation");
        WScript.Echo("ALL PASSED");
        WScript.Quit(0);
    }

    WScript.Echo("message-bus-designer: " + fails.length + " FAILURE(S)");
    for (var i = 0; i < fails.length; ++i) {
        WScript.Echo("  [FAIL] " + fails[i]);
    }
    WScript.Quit(1);
})();
