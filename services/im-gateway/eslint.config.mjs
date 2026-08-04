import eslint from '@eslint/js';
import tseslint from 'typescript-eslint';

export default tseslint.config({ ignores: ['dist/**'] }, eslint.configs.recommended, tseslint.configs.recommended, {
    files: ['src/**/*.ts'],
    rules: {
        '@typescript-eslint/consistent-type-imports': ['error', { fixStyle: 'inline-type-imports' }],
        '@typescript-eslint/no-unused-vars': ['error', { argsIgnorePattern: '^_' }],
    },
});
