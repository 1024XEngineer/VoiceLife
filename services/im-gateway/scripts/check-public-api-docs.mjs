import { readdir, readFile } from 'node:fs/promises';
import { dirname, join, relative, resolve } from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import ts from 'typescript';

const CJK_PATTERN = /[\u3400-\u4dbf\u4e00-\u9fff]/;
const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const packageRoot = resolve(scriptDirectory, '..');
const sourceRoot = join(packageRoot, 'src');

function isExported(node) {
    return node.modifiers?.some((modifier) => modifier.kind === ts.SyntaxKind.ExportKeyword) ?? false;
}

function declarationKind(node) {
    if (ts.isClassDeclaration(node)) return 'class';
    if (ts.isInterfaceDeclaration(node)) return 'interface';
    if (ts.isTypeAliasDeclaration(node)) return 'type';
    if (ts.isFunctionDeclaration(node)) return 'function';
    if (ts.isEnumDeclaration(node)) return 'enum';
    if (ts.isVariableStatement(node)) return 'const';
    return undefined;
}

function declarationNames(node) {
    if (ts.isVariableStatement(node)) {
        return node.declarationList.declarations.flatMap((declaration) =>
            ts.isIdentifier(declaration.name) ? [declaration.name.text] : [],
        );
    }
    return node.name === undefined ? [] : [node.name.text];
}

function precedingTsDoc(sourceFile, node) {
    const leadingTrivia = sourceFile.text.slice(node.getFullStart(), node.getStart(sourceFile));
    const match = /\/\*\*[\s\S]*?\*\/\s*$/.exec(leadingTrivia);
    return match?.[0].trim();
}

function functionParameters(node) {
    return node.parameters.flatMap((parameter) => (ts.isIdentifier(parameter.name) ? [parameter.name.text] : []));
}

function returnsValue(node) {
    if (ts.isConstructorDeclaration(node) || ts.isSetAccessorDeclaration(node)) return false;
    return node.type?.kind !== ts.SyntaxKind.VoidKeyword;
}

function isPublic(node) {
    return !(
        node.modifiers?.some(
            (modifier) =>
                modifier.kind === ts.SyntaxKind.PrivateKeyword || modifier.kind === ts.SyntaxKind.ProtectedKeyword,
        ) ?? false
    );
}

function documentedMembers(node) {
    if (ts.isInterfaceDeclaration(node)) {
        return node.members.filter(ts.isMethodSignature);
    }
    if (ts.isClassDeclaration(node)) {
        return node.members.filter(
            (member) =>
                isPublic(member) &&
                (ts.isConstructorDeclaration(member) ||
                    ts.isMethodDeclaration(member) ||
                    ts.isGetAccessorDeclaration(member) ||
                    ts.isSetAccessorDeclaration(member)),
        );
    }
    return [];
}

function memberName(node) {
    if (ts.isConstructorDeclaration(node)) return 'constructor';
    return node.name?.getText() ?? '<anonymous>';
}

function hasInheritDoc(doc) {
    return /\{@inheritDoc\s+[^}]+\}/.test(doc);
}

function checkCallableDoc(filePath, sourceFile, node, kind, name, allowInheritDoc = false) {
    const lineNumber = sourceFile.getLineAndCharacterOfPosition(node.getStart(sourceFile)).line + 1;
    const doc = precedingTsDoc(sourceFile, node);
    if (doc === undefined) {
        return [`${filePath}:${lineNumber}: ${kind} ${name} 缺少紧邻的 /** ... */ TSDoc 注释`];
    }
    if (allowInheritDoc && hasInheritDoc(doc)) return [];

    const errors = [];
    if (!CJK_PATTERN.test(doc)) {
        errors.push(`${filePath}:${lineNumber}: ${kind} ${name} 的 TSDoc 必须包含中文职责说明`);
    }
    for (const parameter of functionParameters(node)) {
        if (!new RegExp(`@param\\s+${parameter}\\b`).test(doc)) {
            errors.push(`${filePath}:${lineNumber}: ${kind} ${name} 缺少 @param ${parameter}`);
        }
    }
    if (returnsValue(node) && !/@returns\b/.test(doc)) {
        errors.push(`${filePath}:${lineNumber}: ${kind} ${name} 缺少 @returns`);
    }
    return errors;
}

/**
 * 检查单个 TypeScript 源文件中的导出 API 文档。
 * @param filePath 用于错误定位的源文件路径。
 * @param sourceText TypeScript 源码文本。
 * @returns 检查发现的错误列表。
 */
export function checkSourceText(filePath, sourceText) {
    const sourceFile = ts.createSourceFile(filePath, sourceText, ts.ScriptTarget.Latest, true, ts.ScriptKind.TS);
    const errors = [];

    for (const node of sourceFile.statements) {
        const kind = declarationKind(node);
        if (kind === undefined || !isExported(node)) continue;

        const lineNumber = sourceFile.getLineAndCharacterOfPosition(node.getStart(sourceFile)).line + 1;
        const names = declarationNames(node);
        const doc = precedingTsDoc(sourceFile, node);
        if (ts.isFunctionDeclaration(node)) {
            errors.push(...checkCallableDoc(filePath, sourceFile, node, '导出函数', names[0] ?? '<anonymous>'));
        } else {
            for (const name of names) {
                if (doc === undefined) {
                    errors.push(`${filePath}:${lineNumber}: 导出的 ${kind} ${name} 缺少紧邻的 /** ... */ TSDoc 注释`);
                    continue;
                }
                if (!CJK_PATTERN.test(doc)) {
                    errors.push(`${filePath}:${lineNumber}: 导出的 ${kind} ${name} 的 TSDoc 必须包含中文职责说明`);
                }
            }
        }

        for (const member of documentedMembers(node)) {
            errors.push(
                ...checkCallableDoc(
                    filePath,
                    sourceFile,
                    member,
                    ts.isInterfaceDeclaration(node) ? '接口方法' : '类公开成员',
                    `${names[0] ?? '<anonymous>'}.${memberName(member)}`,
                    ts.isClassDeclaration(node),
                ),
            );
        }
    }

    return errors;
}

async function typescriptFiles(directory) {
    const entries = await readdir(directory, { withFileTypes: true });
    const files = await Promise.all(
        entries.map(async (entry) => {
            const path = join(directory, entry.name);
            if (entry.isDirectory()) return typescriptFiles(path);
            return entry.isFile() && entry.name.endsWith('.ts') && !entry.name.endsWith('.d.ts') ? [path] : [];
        }),
    );
    return files.flat().sort();
}

/**
 * 全量检查源码目录中的 TypeScript 导出 API 文档。
 * @param directory 要递归扫描的源码目录。
 * @returns 检查发现的错误列表。
 */
export async function checkSourceTree(directory = sourceRoot) {
    const files = await typescriptFiles(directory);
    const errors = [];
    for (const file of files) {
        const displayPath = relative(packageRoot, file);
        errors.push(...checkSourceText(displayPath, await readFile(file, 'utf8')));
    }
    return { errors, fileCount: files.length };
}

async function main() {
    const { errors, fileCount } = await checkSourceTree();
    if (errors.length > 0) {
        console.error(errors.join('\n'));
        process.exitCode = 1;
        return;
    }
    console.log(`PASS 已全量检查 ${fileCount} 个 TypeScript 源文件的导出 API TSDoc 注释`);
}

if (process.argv[1] !== undefined && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    await main();
}
