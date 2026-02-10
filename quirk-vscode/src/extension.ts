import * as vscode from 'vscode';
import { QuirkDefinitionProvider } from './ImportProvider';
import { QuirkCompletionProvider } from './CompletionProvider';

export function activate(context: vscode.ExtensionContext) {
    
    const selector = { language: 'quirk', scheme: 'file' };

    // Register Definition Provider
    context.subscriptions.push(
        vscode.languages.registerDefinitionProvider(
            selector, 
            new QuirkDefinitionProvider()
        )
    );

    // Register Completion Provider
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            selector,
            new QuirkCompletionProvider(),
            '.', // Trigger on dot (for use core.)
            '{', // Trigger on brace (for use { )
            ',', // Trigger on comma (for use { A, )
            ' '  // Trigger on space (generic)
        )
    );
}

export function deactivate() {}