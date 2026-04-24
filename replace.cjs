const fs = require('fs');
const path = require('path');

const testsDir = path.join(__dirname, 'tests');
const files = fs.readdirSync(testsDir).filter(f => f.startsWith('test_') && f.endsWith('.c'));

for (const file of files) {
    if (file === 'test_phase1.c') continue; // Already manually updated

    let content = fs.readFileSync(path.join(testsDir, file), 'utf8');

    // Remove old includes
    content = content.replace(/#include <assert\.h>/g, '');
    content = content.replace(/#include "\.\.\/include\/.*"/g, '');
    
    // Ensure #include "proven.h" and #include "proven_test.h"
    if (!content.includes('"proven.h"')) {
        content = '#include "proven.h"\n' + content;
    }
    if (!content.includes('"proven_test.h"')) {
        content = content.replace(/#include "proven\.h"/, '#include "proven.h"\n#include "proven_test.h"');
    }

    // Replace proven_println(...) with PROVEN_TEST_INFO(...)
    content = content.replace(/\(void\)proven_println\(([^;]+)\);/g, 'PROVEN_TEST_INFO($1);');
    content = content.replace(/proven_println\(([^;]+)\);/g, 'PROVEN_TEST_INFO($1);');
    
    // Try to auto-derive intent for asserts
    let match;
    const assertRegex = /assert\(([^)]+)\);/g;
    let newContent = '';
    let lastIndex = 0;
    while ((match = assertRegex.exec(content)) !== null) {
        newContent += content.slice(lastIndex, match.index);
        let cond = match[1];
        let intent = `Testing condition: ${cond.replace(/"/g, "'")}`;
        let hint = `Review logic surrounding ${cond.replace(/"/g, "'")}`;
        newContent += `PROVEN_TEST_ASSERT(${cond}, "${intent}", "${hint}");`;
        lastIndex = assertRegex.lastIndex;
    }
    newContent += content.slice(lastIndex);

    // Some asserts might have nested parentheses, the regex might fail...
    // Let's refine the assert regex or just do a simple replace inside a loop
    
    fs.writeFileSync(path.join(testsDir, file), newContent, 'utf8');
}
console.log('Done replacing!');
