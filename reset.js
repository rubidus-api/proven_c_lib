import { execSync } from 'child_process';
try {
  execSync('git reset --hard', { stdio: 'inherit' });
} catch (e) {
  console.log(e.message);
}
