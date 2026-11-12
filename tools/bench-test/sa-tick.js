// The tick for a built view that has none (SeverActions' main view): it hands the page one inventory payload, then
// every 20 frames clicks the next control of the page's navigation, so the page re-renders as a player moving between
// pages would make it. With no game behind it the other pages show their empty states.
(function () {
    var k = 0;
    window.__benchClicks = 0;
    window.__bench = function (n) {
        if (n === 5 && window.receivePageData) {
            var items = [];
            for (var i = 0; i < 120; i++)
                items.push({ formId: 4096 + i, name: 'Item ' + i, count: 1 + i % 5, weight: i % 9, value: i * 3, type: 'misc' });
            window.receivePageData(JSON.stringify({
                page: 'inventory', actors: [{ formId: 20, name: 'Prisoner', carryWeight: { current: 12, max: 300 } }],
                selectedActorFormId: 20, selectedActorName: 'Prisoner', goldCount: 0, categories: { misc: items },
                totalItems: items.length, totalWeight: 12, totalValue: 0,
                stats: { level: 1, race: 'Nord', sex: 'Male', className: 'Spellsword', health: 100, magicka: 100, stamina: 100,
                         perkCount: 1, perkPoints: 0, skills: { oneHanded: 20, twoHanded: 25, archery: 15, block: 20 } }
            }));
        }
        if (n % 20) return;
        var c = document.querySelectorAll('nav button, nav a, aside button, [role="tab"]');
        if (!c.length) return;
        c[k++ % c.length].click();
        window.__benchClicks++;
    };
})();
