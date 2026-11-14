-- SPDX-License-Identifier: Apache-2.0
-- Real Busted 2.2.0 DSL, including a literal-pattern selection counterexample.
describe('selection', function()
    it('passes', function()
        local marker = os.getenv('FROST_LUA_TEST_BODY_MARKER')
        if marker then local file = assert(io.open(marker, 'wb')); assert(file:write('ran')); assert(file:close()) end
        assert.are.equal(42, 6*7)
    end)
    it('fails', function() assert.are.equal(1, 2) end)
    pending('skips')
    it('literal[x]%-?', function() assert.is_true(true) end)
    it('literalx-other', function() error('must not run for literal selection') end)
end)
