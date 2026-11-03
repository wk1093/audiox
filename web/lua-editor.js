(function (root) {
  'use strict';

  const keywords = new Set(('and break do else elseif end false for function goto if in local nil not or repeat return then true until while').split(' '));
  const builtins = new Set(('assert error ipairs next pairs pcall select tonumber tostring type xpcall math string table utf8 hook log state event sound midi effects volume config settings system bluetooth script_config storage ui tap press release wait light millis epoch').split(' '));
  const escape = (text) => text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

  function highlight(source) {
    let result = '';
    let offset = 0;
    while (offset < source.length) {
      const rest = source.slice(offset);
      let kind = '';
      let length = 1;
      const comment = rest.startsWith('--');
      const bracket = /^(?:--)?\[(=*)\[/.exec(rest);
      if (bracket) {
        const close = source.indexOf(`]${bracket[1]}]`, offset + bracket[0].length);
        length = close < 0 ? rest.length : close + bracket[1].length + 2 - offset;
        kind = comment ? 'comment' : 'string';
      } else if (comment) {
        const end = rest.indexOf('\n');
        length = end < 0 ? rest.length : end;
        kind = 'comment';
      } else if (rest[0] === '"' || rest[0] === "'") {
        const quote = rest[0];
        while (length < rest.length) {
          const character = rest[length++];
          if (character === '\\') {
            if (length < rest.length) length++;
          } else if (character === quote || character === '\n') break;
        }
        kind = 'string';
      } else if (rest.startsWith('..')) {
        length = rest.startsWith('...') ? 3 : 2;
        kind = 'operator';
      } else {
        const identifier = /^[A-Za-z_][A-Za-z_0-9]*/.exec(rest);
        const number = /^(?:0[xX](?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)(?:[pP][+-]?\d+)?|(?:\d+(?:\.(?!\.)\d*)?|\.\d+)(?:[eE][+-]?\d+)?)/.exec(rest);
        if (identifier) {
          length = identifier[0].length;
          kind = keywords.has(identifier[0]) ? 'keyword' : builtins.has(identifier[0]) ? 'builtin' : '';
        } else if (number) {
          length = number[0].length;
          kind = 'number';
        } else if (/^[+\-*/%^#=<>~&|:]/.test(rest)) {
          kind = 'operator';
        }
      }
      const text = escape(source.slice(offset, offset + length));
      result += kind ? `<span class="lua-${kind}">${text}</span>` : text;
      offset += length;
    }
    return result;
  }

  function attach(textarea, preview) {
    const syncScroll = () => {
      preview.scrollTop = textarea.scrollTop;
      preview.scrollLeft = textarea.scrollLeft;
    };
    const refresh = () => {
      preview.innerHTML = highlight(textarea.value) + '\n';
      syncScroll();
    };
    textarea.addEventListener('input', refresh);
    textarea.addEventListener('lua-highlight', refresh);
    textarea.addEventListener('scroll', syncScroll);
    refresh();
    textarea.parentElement.classList.add('lua-highlighted');
    return refresh;
  }

  function setValue(textarea, source) {
    textarea.value = source;
    textarea.dispatchEvent(new Event('lua-highlight'));
  }

  const api = { highlight, attach, setValue };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else {
    root.AudioxLuaEditor = api;
    attach(document.getElementById('chord-script'), document.getElementById('lua-preview'));
  }
})(typeof globalThis !== 'undefined' ? globalThis : this);
