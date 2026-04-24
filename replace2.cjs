const fs = require('fs');
const path = require('path');

const testsDir = path.join(__dirname, 'tests');
const files = fs.readdirSync(testsDir).filter(f => f.startsWith('test_') && f.endsWith('.c'));

for (const file of files) {
    if (file === 'test_phase1.c') continue; 
    let content = fs.readFileSync(path.join(testsDir, file), 'utf8');

    // Replace proven_println(...) with PROVEN_TEST_INFO(...)
    content = content.replace(/\(void\)proven_println\(([^;]+)\);/g, 'PROVEN_TEST_INFO($1);');
    content = content.replace(/proven_println\(([^;]+)\);/g, 'PROVEN_TEST_INFO($1);');
    
    // Manual pass to parse 'assert(...)' and handle nested parentheses
    let result = '';
    let i = 0;
    while (i < content.length) {
        if (content.substring(i, i + 7) === 'assert(') {
            let depth = 1;
            let start = i + 7;
            let end = start;
            while(end < content.length && depth > 0) {
                if (content[end] === '(') depth++;
                else if (content[end] === ')') depth--;
                end++;
            }
            if (depth === 0) {
                let cond = content.substring(start, end - 1);
                let intent = `Testing condition: ${cond.replace(/"/g, "'")}`;
                let hint = `Review logic surrounding ${cond.replace(/"/g, "'")}`;
                result += `PROVEN_TEST_ASSERT(${cond}, "${intent}", "${hint}")`;
                i = end;
            } else {
                result += content[i];
                i++;
            }
        } else {
            result += content[i];
            i++;
        }
    }

    fs.writeFileSync(path.join(testsDir, file), result, 'utf8');
}
console.log('Done replacing nested asserts!');
