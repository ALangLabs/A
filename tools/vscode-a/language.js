'use strict';

// A small lexical scan for structural editor features, not a second parser or
// typechecker. A is Python-style: `#` comments, significant indentation,
// triple-quoted strings. Offsets stay in UTF-16, as TextDocument.positionAt
// requires.

const DECL_KINDS = {
    fn: 'Function', struct: 'Struct', class: 'Class', enum: 'Enum',
    trait: 'Interface', type: 'TypeParameter'
};

// Top-level declarations: `fn name`, `struct Name:`, `class Name:`,
// `enum Name:`, `trait Name:`, `type Name = ...` at zero indentation.
// Returns {name, kind, detail, start, end, nameStart, nameEnd} offsets.
function declarations(source) {
    const result = [];
    let offset = 0;
    for (const line of source.split('\n')) {
        const match = /^(fn|struct|class|enum|trait|type)\s+([A-Za-z_]\w*)/.exec(line);
        if (match && DECL_KINDS[match[1]]) {
            const nameStart = offset + match[0].length - match[2].length;
            result.push({
                name: match[2],
                kind: DECL_KINDS[match[1]],
                detail: match[1],
                start: offset,
                end: offset + line.length,
                nameStart,
                nameEnd: nameStart + match[2].length
            });
        }
        offset += line.length + 1;
    }
    return result;
}

// Import statements: `import "path"`, `import "path" as x`,
// `from "path" import ...`, `import name`, `from name import ...`.
// Returns {quoted, spec, line, startChar, endChar} per statement, where the
// character span covers the module specifier (the quoted path without quotes,
// or the bare name).
function imports(source) {
    const result = [];
    const lines = source.split('\n');
    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        let match = /^\s*import\s+"([^"]+)"/.exec(line) || /^\s*from\s+"([^"]+)"\s+import\b/.exec(line);
        if (match) {
            const at = line.indexOf('"', line.indexOf(match[1]) - 1);
            result.push({
                quoted: true, spec: match[1], line: i,
                startChar: at + 1, endChar: at + 1 + match[1].length
            });
            continue;
        }
        match = /^\s*import\s+([A-Za-z_]\w*)\s*(?:#.*)?$/.exec(line) ||
                /^\s*from\s+([A-Za-z_]\w*)\s+import\b/.exec(line);
        if (match) {
            const startChar = line.indexOf(match[1]);
            result.push({
                quoted: false, spec: match[1], line: i,
                startChar, endChar: startChar + match[1].length
            });
        }
    }
    return result;
}

module.exports = { declarations, imports };
