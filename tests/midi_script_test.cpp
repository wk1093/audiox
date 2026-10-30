#include "midi/script.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
MidiScriptPlan plan{};
MidiScriptState state{};
char error[256];
const MidiScriptEvent defaultEvent{0, 0, 127, true};

const MidiScriptStateEntry *findState(const MidiScriptState &value, const char *key) {
    for (unsigned i = 0; i < value.count; ++i)
        if (std::strcmp(value.entries[i].key, key) == 0) return &value.entries[i];
    return nullptr;
}

void success(const char *source) {
    std::memset(error, '!', sizeof(error));
    if (!midiCompileScript(source, plan, error, sizeof(error))) {
        std::fprintf(stderr, "Unexpected failure: %s\nSource: %s\n", error, source);
        assert(false);
    }
    assert(!error[0]);
}

void evaluationSuccess(const char *source, const MidiScriptEvent &event = defaultEvent) {
    std::memset(error, '!', sizeof(error));
    if (!midiEvaluateScript(source, event, state, plan, error, sizeof(error))) {
        std::fprintf(stderr, "Unexpected evaluation failure: %s\nSource: %s\n", error, source);
        assert(false);
    }
    assert(!error[0]);
}

void failure(const char *source, const char *expected) {
    std::memset(&plan, 0x7f, sizeof(plan));
    std::memset(error, '!', sizeof(error));
    bool result = midiCompileScript(source, plan, error, sizeof(error));
    if (result || !std::strstr(error, expected)) {
        std::fprintf(stderr, "Expected failure containing '%s', got %s: %s\n",
                     expected, result ? "success" : "failure", error);
        assert(false);
    }
    assert(plan.count == 0);
    for (const MidiScriptAction &action : plan.actions) {
        assert(action.kind == MidiScriptKind::Tap && action.value == 0);
        for (char key : action.keys) assert(!key);
        for (char character : action.text) assert(!character);
        for (uint8_t byte : action.midiBytes) assert(!byte);
        assert(!action.midiSize && action.pitchRatio == 0);
    }
}

void evaluationFailure(const char *source, const char *expected) {
    MidiScriptState before = state;
    std::memset(&plan, 0x7f, sizeof(plan));
    std::memset(error, '!', sizeof(error));
    bool result = midiEvaluateScript(source, defaultEvent, state, plan, error, sizeof(error));
    if (result || !std::strstr(error, expected)) {
        std::fprintf(stderr, "Expected eval failure containing '%s', got %s: %s\n",
                     expected, result ? "success" : "failure", error);
        assert(false);
    }
    assert(plan.count == 0);
    assert(std::memcmp(&before, &state, sizeof(state)) == 0);
}
}

int main() {
    std::string source;
    success("");
    assert(plan.count == 0);
    success("tap('CTRL+A'); wait(100); light(127)");
    assert(plan.count == 3);
    assert(plan.actions[0].kind == MidiScriptKind::Tap);
    assert(std::strcmp(plan.actions[0].keys, "CTRL+A") == 0);
    assert(plan.actions[0].value == 0);
    assert(plan.actions[1].kind == MidiScriptKind::Wait && plan.actions[1].value == 100);
    assert(plan.actions[2].kind == MidiScriptKind::Light && plan.actions[2].value == 127);
    success("local n = 0; for i=1,3 do if i%2 == 1 then tap('SHIFT+F13') "
            "else light(i) end; wait(i*10); n=n+1 end; if n==3 then light(0) end");
    assert(plan.count == 7);
    assert(plan.actions[2].kind == MidiScriptKind::Light && plan.actions[2].value == 2);
    assert(plan.actions[5].value == 30 && plan.actions[6].value == 0);
    success("local function chord(k) tap(k) end; local a={'A','B'}; "
            "local i=1; while i<=2 do chord(a[i]); i=i+1 end");
    assert(plan.count == 2 && std::strcmp(plan.actions[1].keys, "B") == 0);
    success("wait(0); wait(60000); light(0); light(127)");
    assert(plan.count == 4 && plan.actions[1].value == 60000);
    success("for i=1,64 do tap('A') end");
    assert(plan.count == MIDI_SCRIPT_ACTIONS_MAX);
    success("for i=1,60 do wait(1000) end");
    assert(plan.count == 60);
    success("return 1, 'ignored', {}");
    assert(plan.count == 0);
    success("press('CTRL'); press('SHIFT+ALT'); release()");
    assert(plan.count == 3);
    assert(plan.actions[0].kind == MidiScriptKind::Press &&
           std::strcmp(plan.actions[0].keys, "CTRL") == 0);
    assert(plan.actions[1].kind == MidiScriptKind::Press);
    assert(plan.actions[2].kind == MidiScriptKind::Release);

    success("type('A\\tB\\n~', 30); type('Z')");
    assert(plan.count == 2);
    assert(plan.actions[0].kind == MidiScriptKind::Type && plan.actions[0].value == 30);
    assert(std::strcmp(plan.actions[0].text, "A\tB\n~") == 0);
    assert(plan.actions[1].kind == MidiScriptKind::Type && plan.actions[1].value == 50);
    success("type('')");
    assert(plan.count == 1 && !plan.actions[0].text[0]);
    std::string fullAscii;
    for (unsigned char c = 32; c <= 126; ++c) fullAscii.push_back(static_cast<char>(c));
    source = "type('";
    for (unsigned char c : fullAscii) {
        source.push_back('\\');
        source.push_back(static_cast<char>('0' + c / 100));
        source.push_back(static_cast<char>('0' + (c / 10) % 10));
        source.push_back(static_cast<char>('0' + c % 10));
    }
    source += "')";
    success(source.c_str());
    assert(std::strcmp(plan.actions[0].text, fullAscii.c_str()) == 0);
    success(("type('" + std::string(255, 'x') + "')").c_str());
    assert(std::strlen(plan.actions[0].text) == 255);
    success("type('A', 30); wait(59970)");
    assert(plan.count == 2);
    success("type('A', 60000)");
    assert(plan.actions[0].value == 60000);

    failure(nullptr, "null");
    failure("tap('A'); tap('NO_SUCH_KEY')", "invalid keyboard chord");
    failure("tap('CTRL')", "invalid keyboard chord");
    failure("tap('CTRL+')", "invalid keyboard chord");
    failure("tap('A\\0B')", "invalid keyboard chord");
    failure("tap('')", "invalid keyboard chord");
    failure("tap('ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKL')",
            "invalid keyboard chord");
    failure("tap(1)", "string");
    failure("tap({})", "string");
    failure("tap(nil)", "string");
    failure("tap()", "exactly one");
    failure("tap('A','B')", "exactly one");
    failure("press('NO_SUCH_KEY')", "invalid keyboard chord");
    failure("press('CTRL+')", "invalid keyboard chord");
    failure("press(1)", "string");
    failure("release(1)", "no arguments");
    failure("release(nil)", "no arguments");
    failure("wait('1')", "integer");
    failure("wait(1.0)", "integer");
    failure("wait(1.5)", "integer");
    failure("wait(false)", "integer");
    failure("wait(nil)", "integer");
    failure("wait()", "exactly one");
    failure("wait(1,2)", "exactly one");
    failure("wait(-1)", "0..60000");
    failure("wait(60001)", "0..60000");
    failure("wait(9223372036854775807)", "0..60000");
    failure("wait(1/0)", "integer");
    failure("wait(0/0)", "integer");
    failure("wait(60000); wait(1)", "total duration limit");
    failure("for i=1,61 do wait(1000) end", "total duration limit");
    failure("type('A', 30); wait(59971)", "total duration limit");
    failure("light('1')", "integer");
    failure("light(1.0)", "integer");
    failure("light(true)", "integer");
    failure("light()", "exactly one");
    failure("light(1,2)", "exactly one");
    failure("light(-1)", "0..127");
    failure("light(128)", "0..127");
    failure("light(9223372036854775807)", "0..127");
    failure("for i=1,65 do light(1) end", "action limit");
    failure("type(1)", "string");
    failure("type()", "string");
    failure("type('A', 1.0)", "integer");
    failure("type('A', -1)", "30..60000");
    failure("type('A', 29)", "30..60000");
    failure("type('A', 60001)", "30..60000");
    failure("type('A', 30, 2)", "string");
    failure("type('A\\0B')", "NUL");
    failure("type('\\r')", "US ASCII");
    failure("type('\\1')", "US ASCII");
    failure("type('\\127')", "US ASCII");
    failure("type('\\195\\169')", "US ASCII");
    failure(("type('" + std::string(256, 'x') + "')").c_str(), "255 bytes");
    failure("type(string.rep('x', 256))", "255 bytes");
    failure(("type('A'); type('" + std::string(256, 'x') + "')").c_str(), "255 bytes");
    failure(("type('" + std::string(129, 'x') + "', 60000)").c_str(),
            "total duration limit");
    failure(("type('" + std::string(257, 'x') + "')").c_str(), "255 bytes");
    failure(("type('" + std::string(200, 'x') + "'); type('" +
             std::string(57, 'y') + "')").c_str(), "total type character limit");
    failure("tap('A'); if then", "macro");
    failure("tap('A'); local x=nil; x()", "nil value");
    failure("while true do end", "instruction limit");
    failure("local function f() return f() end; f()", "instruction limit");
    failure("local s='xxxxxxxx'; for i=1,20 do s=s..s end", "memory limit");
    failure("\x1bLua", "bytecode");

    source.assign(MIDI_SCRIPT_MAX - 1, ' ');
    success(source.c_str());
    source += ' ';
    failure(source.c_str(), "less than 2048");
    char unterminated[MIDI_SCRIPT_MAX];
    std::memset(unterminated, ' ', sizeof(unterminated));
    failure(unterminated, "less than 2048");

    const char *globals[] = {
        "_G", "_VERSION", "pcall", "xpcall", "print", "warn", "collectgarbage",
        "getmetatable", "setmetatable", "rawget", "rawset", "rawequal", "rawlen",
        "load", "loadfile", "dofile", "require", "os", "io", "package", "debug",
        "coroutine", "utf8"
    };
    for (const char *name : globals) {
        source = std::string("if ") + name + " ~= nil then tap('A') end";
        success(source.c_str());
        assert(plan.count == 0);
        source = std::string(name) + "()";
        failure(source.c_str(), "nil value");
    }
    success("if event ~= nil or state ~= nil or type ~= nil then light(1) end");
    assert(plan.count == 1 && plan.actions[0].kind == MidiScriptKind::Light);
    success("tap=nil; leaked=123");
    success("if leaked ~= nil then light(1) end; tap('A')");
    assert(plan.count == 1 && plan.actions[0].kind == MidiScriptKind::Tap);
    assert(midiValidateScript("while true do end", error, sizeof(error)));
    assert(midiValidateScript("tap('A'); local x = {", error, sizeof(error)) == false);
    assert(std::strstr(error, "macro"));

    success("assert(value_type(type)=='function'); assert(value_type(1)=='number'); "
            "assert(value_type({})=='table'); assert(tonumber('ff',16)==255); "
            "assert(tonumber('no')==nil); assert(tostring(123)=='123'); "
            "assert(select('#',1,nil,3)==3); assert(select(2,1,7)==7); "
            "local t={3,4}; local n=0; for i,v in ipairs(t) do n=n+i*v end; assert(n==11); "
            "n=0; for k,v in pairs({a=3,b=4}) do n=n+v end; assert(n==7); "
            "assert(next({})==nil); assert(assert(true,7)==true)");
    failure("log('not delivered'); error('intentional')", "intentional");
    failure("assert(false,'assertion test')", "assertion test");
    success("assert(string.dump==nil and ('').dump==nil); "
            "for _,k in ipairs({'find','gsub','match','gmatch','pack','unpack','packsize'}) do "
            "assert(string[k]==nil and ('')[k]==nil) end; "
            "assert(table.sort==nil and table.pack==nil); "
            "assert(('AbC'):lower()=='abc'); assert(string.upper('ab')=='AB'); "
            "assert(string.sub('abcde',-3,-2)=='cd'); assert(string.reverse('abc')=='cba'); "
            "assert(string.len('a\\0b')==3); assert(string.char(65,0,66)=='A\\0B'); "
            "local a,b=string.byte('AZ',1,2); assert(a==65 and b==90); "
            "assert(string.rep('xy',3,'-')=='xy-xy-xy'); "
            "assert(string.format('%04d %.2f %s',7,1.25,'hi')=='0007 1.25 hi')");
    success("local t={'A','C'}; table.insert(t,2,'B'); assert(table.concat(t,',')=='A,B,C'); "
            "assert(table.remove(t,2)=='B'); assert(table.concat(t)=='AC'); "
            "table.move(t,1,2,2); assert(table.concat(t)=='AAC'); "
            "local d={}; assert(table.move(t,1,2,1,d)==d); "
            "local a,b=table.unpack(d); assert(a=='A' and b=='A'); "
            "table.insert(d,'Z'); assert(table.remove(d)=='Z'); "
            "assert(table.concat({1,2,3},'-',2,3)=='2-3')");
    success("assert(math.abs(-3)==3 and math.floor(2.9)==2 and math.ceil(2.1)==3); "
            "assert(math.min(4,2,8)==2 and math.max(4,2,8)==8); "
            "assert(math.sqrt(9)==3 and math.fmod(7,3)==1); "
            "assert(math.tointeger(3.0)==3 and math.tointeger(3.2)==nil); "
            "assert(math.type(2)=='integer' and math.type(2.0)=='float' and math.type('2')==nil); "
            "assert(math.ult(1,-1)); local a,b=math.modf(1.25); assert(a==1 and b==0.25); "
            "assert(math.sin(0)==0 and math.cos(0)==1 and math.tan(0)==0); "
            "assert(math.asin(0)==0 and math.acos(1)==0 and math.atan(0)==0); "
            "assert(math.log(1)==0 and math.exp(0)==1 and math.log(8,2)==3); "
            "assert(math.deg(math.pi)==180 and math.rad(180)==math.pi); "
            "assert(math.huge==1/0 and math.mininteger<0 and math.maxinteger>0); "
            "assert(clamp(3,0,2)==2 and clamp(-1,0,2)==0 and clamp(1,0,2)==1); "
            "assert(map(3,0,2,0,10)==15 and map(1,2,0,0,10)==5); "
            "assert(round(1.235,2)==1.24 and round(-1.5)==-2 and round(1234,-2)==1200)");
    failure("clamp(1,2,0)", "bounds");
    failure("map(1,0,0,0,1)", "range");
    failure("round(1,7)", "-6..6");
    failure("round(1,1.0)", "integer");
    failure("round(1/0)", "finite");
    failure("clamp('1',0,2)", "finite");
    failure("map(1,0,2,0/0,1)", "finite");
    failure("math.fmod(2,0)", "zero divisor");
    failure("math.sin(1/0)", "finite");

    success("math.randomseed(42); local a=math.random(); local b=math.random(-9,9); "
            "math.randomseed(42); assert(math.random()==a and math.random(-9,9)==b); "
            "for i=1,100 do local n=math.random(3); assert(n>=1 and n<=3); "
            "n=math.random(); assert(n>=0 and n<1) end; "
            "assert(math.random(7,7)==7); "
            "local n=math.random(math.mininteger,math.maxinteger); assert(math.type(n)=='integer'); "
            "local t={10,20,30}; for i=1,20 do local n=choose(t); assert(n==10 or n==20 or n==30) end");
    failure("math.random(0)", "empty random");
    failure("math.random(3,2)", "empty random");
    failure("math.random(3.0)", "integer");
    failure("math.randomseed()", "argument count");
    failure("math.randomseed(1.0)", "integer");
    failure("choose({})", "dense array");
    failure("choose({1,nil,3})", "dense array");
    failure("choose({1,extra=2})", "dense array");
    failure("choose({[1000000]=1})", "dense array");
    success("log(string.format('hit %d',1)); sound.play('kick.wav'); "
            "sound.play('snare.wav',0.25); sound.play('hat.wav',4); sound.stop_all()");
    assert(plan.count == 5 && plan.actions[0].kind == MidiScriptKind::Log);
    assert(std::strcmp(plan.actions[0].text, "hit 1") == 0);
    assert(plan.actions[1].kind == MidiScriptKind::SoundPlay && plan.actions[1].pitchRatio == 1);
    assert(std::strcmp(plan.actions[1].text, "kick.wav") == 0);
    assert(plan.actions[2].pitchRatio == 0.25 && plan.actions[3].pitchRatio == 4);
    assert(plan.actions[4].kind == MidiScriptKind::SoundStopAll);
    success("log(''); log(string.rep('x',255)); sound.play(string.rep('x',159))");
    failure("log(1)", "string");
    failure("log('x',1)", "argument count");
    failure("log('x\\0y')", "no NUL");
    failure("log(string.rep('x',256))", "255 bytes");
    for (const char *name : {"''", "'..'", "'a..b.wav'", "'/kick.wav'", "'a/b.wav'",
                             "'a\\\\b.wav'", "'x\\0y'", "'x\\ny'", "'x\\127y'",
                             "string.rep('x',160)"}) {
        source = std::string("sound.play(") + name + ")";
        failure(source.c_str(), "basename");
    }
    failure("sound.play('x',0.249)", "pitch");
    failure("sound.play('x',4.01)", "pitch");
    failure("sound.play('x',0/0)", "finite");
    failure("sound.play('x','1')", "finite");
    failure("sound.stop_all(1)", "argument count");
    success("midi.note_on(15,127,99); midi.note_off(2,64); midi.note_off(1,63,12); "
            "midi.cc(3,7,100); midi.program(4,8); midi.send(0xf8); midi.send(0xf1,3); "
            "midi.send(0xf2,1,2)");
    assert(plan.count == 8);
    const uint8_t expectedMidi[][4] = {{0x9f,127,99,3},{0x82,64,0,3},{0x81,63,12,3},
        {0xb3,7,100,3},{0xc4,8,0,2},{0xf8,0,0,1},{0xf1,3,0,2},{0xf2,1,2,3}};
    for (unsigned i = 0; i < plan.count; ++i) {
        assert(plan.actions[i].kind == MidiScriptKind::Midi);
        assert(std::memcmp(plan.actions[i].midiBytes, expectedMidi[i], 3) == 0);
        assert(plan.actions[i].midiSize == expectedMidi[i][3]);
    }
    for (int status = 0; status <= 255; ++status) {
        int data = -1;
        if (status >= 0x80 && status <= 0xef) data = (status & 0xe0) == 0xc0 ? 1 : 2;
        else if (status == 0xf1 || status == 0xf3) data = 1;
        else if (status == 0xf2) data = 2;
        else if (status == 0xf6 || status == 0xf8 || status == 0xfa || status == 0xfb ||
                 status == 0xfc || status == 0xfe || status == 0xff) data = 0;
        source = "midi.send(" + std::to_string(status);
        if (data > 0) source += ",0";
        if (data > 1) source += ",127";
        source += ")";
        if (data < 0) failure(source.c_str(), "unsupported MIDI");
        else {
            success(source.c_str());
            assert(plan.actions[0].midiBytes[0] == status && plan.actions[0].midiSize == data + 1);
            source.insert(source.size() - 1, ",0");
            failure(source.c_str(), data == 2 ? "argument count" : "length");
        }
    }
    failure("midi.send(0x90,1)", "length");
    failure("midi.send(0xf1)", "length");
    failure("midi.send(0xc0,128)", "range");
    failure("midi.send(0x90,-1,0)", "range");
    failure("midi.send(0x90,0,1.0)", "integer");
    failure("midi.send(144.0,0,0)", "integer");
    failure("midi.note_on(16,60,1)", "range");
    failure("midi.note_off(-1,60)", "range");
    failure("midi.cc(0,128,0)", "range");
    failure("midi.program(0,128)", "range");
    failure("midi.note_on(0,60)", "argument count");
    failure("midi.program(0,1,2)", "argument count");
    failure("for i=1,65 do log('x') end", "action limit");
    failure("for i=1,65 do sound.stop_all() end", "action limit");
    failure("for i=1,65 do midi.send(0xf8) end", "action limit");
    success("effects.enable('fx_7',false); effects.toggle('fx_7'); "
            "effects.set('fx_7','channels',3); volume.set('sfx',75); volume.adjust('sfx',-5)");
    assert(plan.count == 5);
    assert(plan.actions[0].kind == MidiScriptKind::EffectEnable && plan.actions[0].value == 0);
    assert(plan.actions[1].kind == MidiScriptKind::EffectToggle);
    assert(plan.actions[2].kind == MidiScriptKind::EffectSet &&
           !strcmp(plan.actions[2].target, "fx_7") && !strcmp(plan.actions[2].parameter, "channels") &&
           plan.actions[2].number == 3);
    assert(plan.actions[3].kind == MidiScriptKind::VolumeSet && plan.actions[3].number == 75);
    assert(plan.actions[4].kind == MidiScriptKind::VolumeAdjust && plan.actions[4].number == -5);
    failure("effects.enable('fx_7',1)", "boolean");
    failure("effects.toggle('fx_7',true)", "argument count");
    failure("effects.set('fx_7','gain',math.huge)", "finite");
    failure("effects.set('fx_7','gain','1')", "finite");
    failure("volume.set('sfx',101)", "range");
    failure("volume.set('sfx',-1)", "range");
    failure("volume.adjust('sfx',-101)", "range");
    failure("volume.adjust('sfx',0/0)", "finite");
    failure("volume.set('',1)", "ID");
    failure("effects.toggle(string.rep('x',64))", "ID");
    failure("effects.set('fx_7','ga\\0in',1)", "IDs");
    failure("for i=1,65 do effects.toggle('fx_7') end", "action limit");
    evaluationSuccess("assert(event.source=='midi')");
    MidiScriptEvent webEvent = defaultEvent;
    webEvent.fromWeb = true;
    evaluationSuccess("assert(event.source=='web')", webEvent);

    failure("table.move({},1,1000000000,1)", "span limit");
    failure("table.move({},math.mininteger,math.maxinteger,1)", "span limit");
    failure("table.move({},1,3,math.maxinteger)", "overflow");
    failure("table.unpack({},1,1000000000)", "span limit");
    failure("table.unpack({},math.mininteger,math.maxinteger)", "span limit");
    failure("table.concat({},1,1,1000000000)", "span limit");
    failure("string.rep('',1000000000)", "rep limit");
    failure("string.rep('a',4097)", "rep limit");
    failure("string.format('%1000000000s','x')", "width/precision");
    failure("string.format('%.1000000000f',1)", "width/precision");
    failure("string.format('%*s',1,'x')", "unsupported format");
    failure("string.format('%n',1)", "unsupported format");
    failure("string.format('%s',{})", "scalar arguments");
    failure("string.format(string.rep('%d',17),1)", "field limit");
    failure("string.byte(string.rep('x',1025),1,1025)", "span limit");
    failure("table.concat({'a','b'},string.rep('x',4096))", "string limit");
    failure("local t={}; for i=1,1025 do t[i]=i end; table.insert(t,1)", "table length");
    failure("local t={}; for i=1,1025 do t[i]=i end; choose(t)", "dense array");
    failure("local s=string.rep('x',4096)..'x'; string.lower(s)", "string limit");
    failure("local s=string.rep('1',4096)..'1'; tonumber(s)", "string limit");
    failure("local s=string.rep('1',4096)..'1'; math.tointeger(s)", "string limit");
    failure("local s=string.rep('1',4096)..'1'; select(s,1)", "string limit");
    failure("local s=string.rep('1',4096)..'1'; string.char(s)", "string limit");
    failure("local s=string.rep('1',4096)..'1'; local n=s+1", "string limit");
    failure("for i=1,100 do string.reverse(string.rep('x',1024)) end", "instruction limit");
    success("assert(table.unpack({},2,1)==nil); table.move({},3,1,math.maxinteger); "
            "assert(string.rep('x',-1)==''); assert(string.byte('abc',3,2)==nil)");

    state = {};
    const MidiScriptEvent clockEvent{3,64,99,true,1234567890123ULL,1793329123};
    evaluationSuccess("state.ms=millis(); state.epoch=epoch(); wait(1000); "
                      "assert(millis()==state.ms and epoch()==state.epoch); "
                      "assert(math.type(millis())=='integer' and math.type(epoch())=='integer'); "
                      "state.rand=math.random(); log('clock')", clockEvent);
    assert(findState(state,"ms")->numberValue == 1234567890123.0);
    assert(findState(state,"epoch")->numberValue == 1793329123);
    double firstRandom = findState(state,"rand")->numberValue;
    evaluationSuccess("assert(math.random()==state.rand); state.counter=1", clockEvent);
    assert(findState(state,"rand")->numberValue == firstRandom);
    evaluationSuccess("state.counter=state.counter+1; log(string.format('count %d',state.counter))");
    assert(std::strcmp(plan.actions[0].text,"count 2") == 0);
    state = {};
    for (unsigned count = 1; count <= 2; ++count) {
        evaluationSuccess("state.count=(state.count or 0)+1; type('Pressed '..state.count)");
        source = "Pressed " + std::to_string(count);
        assert(std::strcmp(plan.actions[0].text, source.c_str()) == 0);
        const MidiScriptStateEntry *entry = findState(state, "count");
        assert(entry->numberIsInteger && entry->integerValue == count && entry->numberValue == count);
    }
    evaluationSuccess("state.big=9223372036854775807; state.small=-9223372036854775807-1; "
                      "state.float=3.0");
    assert(findState(state,"big")->numberIsInteger && findState(state,"big")->integerValue == INT64_MAX);
    assert(findState(state,"small")->numberIsInteger && findState(state,"small")->integerValue == INT64_MIN);
    assert(!findState(state,"float")->numberIsInteger);
    evaluationSuccess("assert(state.big==math.maxinteger and state.small==math.mininteger); "
                      "assert(math.type(state.big)=='integer' and math.type(state.small)=='integer'); "
                      "assert(math.type(state.float)=='float'); state.counter=2");
    evaluationFailure("state.counter=99; log('rollback'); sound.play('kick.wav'); "
                      "midi.note_on(0,60,100); error('rollback')", "rollback");
    assert(findState(state,"counter")->numberValue == 2);
    evaluationFailure("log('rollback'); state.bad={}", "state values");
    failure("millis(1)", "argument count");
    failure("epoch(1)", "argument count");
    evaluationSuccess("assert(millis()==0 and epoch()==-1)", MidiScriptEvent{0,0,0,true,0,-1});
    evaluationSuccess("assert(millis()==math.maxinteger)", MidiScriptEvent{0,0,0,true,UINT64_MAX,0});
    success("assert(millis()==0 and epoch()==0)");

    state = {};
    evaluationSuccess("state.hits = (state.hits or 0) + 1; "
                      "state.flag = event.pressed; state.kind = 'note'; "
                      "state.channel = event.channel; if event.pressed and event.note == 64 "
                      "then press('SHIFT') else release() end",
                      MidiScriptEvent{3, 64, 99, true});
    assert(state.count == 4);
    assert(findState(state, "hits")->kind == MidiScriptValueKind::Number &&
           findState(state, "hits")->numberValue == 1);
    assert(findState(state, "flag")->booleanValue);
    assert(std::strcmp(findState(state, "kind")->stringValue, "note") == 0);
    assert(findState(state, "channel")->numberValue == 3);
    assert(plan.count == 1 && plan.actions[0].kind == MidiScriptKind::Press);
    evaluationSuccess("state.hits = state.hits + 1; "
                      "if event.channel == 3 and event.note == 64 and event.velocity == 99 "
                      "and not event.pressed then tap('A') end",
                      MidiScriptEvent{3, 64, 99, false});
    assert(findState(state, "hits")->numberValue == 2);
    assert(plan.count == 1 && plan.actions[0].kind == MidiScriptKind::Tap);
    evaluationFailure("state.hits = 100; tap('A'); state = nil", "state must remain a table");
    assert(findState(state, "hits")->numberValue == 2);
    evaluationFailure("state.bad = {}; tap('A')", "state values");
    evaluationFailure("state[1] = 'x'", "state keys");
    evaluationFailure("state[''] = true", "state keys");
    evaluationFailure(("state['" + std::string(32, 'x') + "'] = true").c_str(), "state keys");
    evaluationFailure(("state.long = '" + std::string(128, 'x') + "'").c_str(), "at most 127");
    evaluationFailure("state.nan = 0/0", "finite");
    evaluationFailure("state.f = function() end", "state values");
    evaluationFailure("for i=1,33 do state['k'..i] = true end", "entry limit");
    evaluationFailure("state.flag = not state.flag; wait(-1)", "0..60000");
    assert(state.count == 4 && findState(state, "flag")->booleanValue);
    evaluationFailure("state = false", "state must remain a table");

    MidiScriptEvent invalidEvent{16, 0, 0, true};
    MidiScriptState stateBefore = state;
    assert(!midiEvaluateScript("tap('A')", invalidEvent, state, plan, error, sizeof(error)));
    assert(std::strstr(error, "event values") && plan.count == 0);
    assert(std::memcmp(&stateBefore, &state, sizeof(state)) == 0);
    MidiScriptState invalidState{};
    invalidState.count = MIDI_SCRIPT_STATE_ENTRIES_MAX + 1;
    assert(!midiEvaluateScript("", defaultEvent, invalidState, plan, error, sizeof(error)));
    assert(std::strstr(error, "state is invalid") && plan.count == 0);
    invalidState = {};
    invalidState.count = 1;
    std::strcpy(invalidState.entries[0].key, "number");
    invalidState.entries[0].kind = MidiScriptValueKind::Number;
    invalidState.entries[0].numberValue = 7;
    assert(midiEvaluateScript("assert(state.number==7 and math.type(state.number)=='float')",
                              defaultEvent, invalidState, plan, error, sizeof(error)));
    invalidState.entries[0].numberIsInteger = true;
    invalidState.entries[0].integerValue = 8;
    assert(!midiEvaluateScript("", defaultEvent, invalidState, plan, error, sizeof(error)));
    assert(std::strstr(error, "state is invalid") && plan.count == 0);

    assert(!midiCompileScript("while true do end", plan, nullptr, 100));
    char tiny[2] = {'!', '!'};
    assert(!midiCompileScript("wait(-1)", plan, tiny, sizeof(tiny)));
    assert(tiny[0] && tiny[1] == '\0');
    char untouched = '!';
    assert(!midiCompileScript("wait(-1)", plan, &untouched, 0));
    assert(untouched == '!');
    for (unsigned i = 0; i < 30; ++i) {
        failure("local s='x'; for i=1,20 do s=s..s end", "memory limit");
        failure("while true do end", "instruction limit");
        success("tap('CTRL+A')");
        assert(plan.count == 1);
    }
    std::puts("MIDI script compiler tests passed");
}
