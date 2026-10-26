/** @env pure @kind contract @why 0x23d / 0x32b 的清理范围（42..999 / 0..999）被改错时当场红 */
import test from 'node:test';
import assert from 'node:assert/strict';

/** 0x23d：只清槽 42..999 的影片对象。 */
function clearMovies(movies) {
  for (const slot of [...movies.keys()]) if (slot >= 42 && slot < 1000) movies.delete(slot);
}

/** 0x32b：清槽 0..999 的网格对象。 */
function clearMeshes(meshes) {
  for (const slot of [...meshes.keys()]) if (slot >= 0 && slot < 1000) meshes.delete(slot);
}

test('★ 0x23d：清影片槽 42..999，保留 0..41 与 1000', () => {
  const movies = new Map([[0, 'a'], [41, 'b'], [42, 'c'], [999, 'd'], [1000, 'e']]);
  clearMovies(movies);
  assert.deepEqual([...movies.keys()], [0, 41, 1000]);
});

test('★ 0x32b：清网格槽 0..999，保留负键与 1000 以上', () => {
  const meshes = new Map([[-1, 'neg'], [0, 'a'], [999, 'b'], [1000, 'c']]);
  clearMeshes(meshes);
  assert.deepEqual([...meshes.keys()], [-1, 1000]);
});
