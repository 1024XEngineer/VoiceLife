import { execFileSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import process from 'node:process';

const declarationPattern = /^export\s+(?:(?:abstract|async)\s+)?(class|interface|type|function|const|enum)\s+(\w+)/;
const repositoryRoot = execFileSync('git', ['rev-parse', '--show-toplevel'], { encoding: 'utf8' }).trim();

function changedLines() {
    const base = process.env.TSDOC_BASE_SHA;
    const revision = base && !/^0+$/.test(base) ? `${base}...HEAD` : 'HEAD';
    const diff = execFileSync('git', ['diff', '--unified=0', '--diff-filter=ACMR', revision, '--', 'src'], {
        encoding: 'utf8',
    });
    const result = new Map();
    let file;
    for (const line of diff.split('\n')) {
        const fileMatch = /^\+\+\+ b\/(.+)$/.exec(line);
        if (fileMatch) {
            file = fileMatch[1];
            result.set(file, new Set());
            continue;
        }
        const hunkMatch = /^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@/.exec(line);
        if (!file || !hunkMatch) continue;
        const start = Number(hunkMatch[1]);
        const count = Number(hunkMatch[2] ?? 1);
        for (let lineNumber = start; lineNumber < start + count; lineNumber += 1) result.get(file).add(lineNumber);
    }
    return result;
}

function precedingDoc(lines, index) {
    let cursor = index - 1;
    while (cursor >= 0 && lines[cursor].trim() === '') cursor -= 1;
    if (cursor < 0 || !lines[cursor].trimEnd().endsWith('*/')) return undefined;

    const comment = [];
    while (cursor >= 0) {
        comment.unshift(lines[cursor]);
        if (lines[cursor].trimStart().startsWith('/**')) return { start: cursor, text: comment.join('\n') };
        cursor -= 1;
    }
    return undefined;
}

function functionParameters(lines, index) {
    const declaration = lines.slice(index, index + 8).join(' ');
    const opening = declaration.indexOf('(');
    const closing = declaration.indexOf(')', opening);
    if (opening === -1 || closing === -1) return [];
    return declaration
        .slice(opening + 1, closing)
        .split(',')
        .map((parameter) => parameter.trim().match(/^(\w+)/)?.[1])
        .filter(Boolean);
}

function returnsValue(lines, index) {
    const declaration = lines.slice(index, index + 8).join(' ');
    const closing = declaration.indexOf(')');
    return closing !== -1 && !/^\s*:\s*void\b/.test(declaration.slice(closing + 1));
}

const errors = [];
for (const [file, changed] of changedLines()) {
    const lines = (await readFile(join(repositoryRoot, file), 'utf8')).split(/\r?\n/);
    for (const [index, line] of lines.entries()) {
        const lineNumber = index + 1;
        const declaration = declarationPattern.exec(line.trim());
        if (!declaration) continue;

        const [, kind, name] = declaration;
        const doc = precedingDoc(lines, index);
        const declarationChanged =
            changed.has(lineNumber) ||
            (doc !== undefined && [...changed].some((changedLine) => changedLine > doc.start && changedLine <= lineNumber));
        if (!declarationChanged) continue;
        if (!doc) {
            errors.push(`${file}:${lineNumber}: 导出的 ${kind} ${name} 缺少紧邻的 /** ... */ TSDoc 注释`);
            continue;
        }
        if (kind !== 'function') continue;
        for (const parameter of functionParameters(lines, index)) {
            if (!new RegExp(`@param\\s+${parameter}\\b`).test(doc.text)) {
                errors.push(`${file}:${lineNumber}: 导出函数 ${name} 缺少 @param ${parameter}`);
            }
        }
        if (returnsValue(lines, index) && !/@returns\b/.test(doc.text)) {
            errors.push(`${file}:${lineNumber}: 导出函数 ${name} 缺少 @returns`);
        }
    }
}

if (errors.length > 0) {
    console.error(errors.join('\n'));
    process.exitCode = 1;
} else {
    console.log('PASS 已检查本次变更的 TypeScript 导出 API TSDoc 注释');
}
