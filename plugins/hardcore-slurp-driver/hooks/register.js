// A separate plugin is required. Claude skips the calling plugin's hooks when
// a timer invokes command.run; using one plugin would bypass its own /bg guard.
export function register(on) {
  on('session.start', async ($, e, next) => {
    await $.command.register({name: 'hardcore-slurp-dispatch',
      description: 'Internal guarded Hardcore handoff dispatcher'});
    return next(e);
  });
  on('command.run', {command: 'hardcore-slurp-dispatch'}, async ($, e) => {
    if (e.origin.kind !== 'plugin' || e.origin.name !== 'hardcore-slurp' ||
        !/^[a-f0-9]{32}$/.test(e.args)) {
      return {text: 'Start handoff with /hardcore-slurp.'};
    }
    const commands = await $.command.list();
    if (!commands.some(command => command.name === 'hardcore-slurp')) {
      return {text: 'The Hardcore handoff guard is not installed; no handoff attempted.'};
    }
    $.clock.after(0, async () => {
      try {
        // A plugin may have been disabled after the initial command handler.
        const current = await $.command.list();
        if (!current.some(command => command.name === 'hardcore-slurp')) {
          await $.ui.log('The Hardcore handoff guard is no longer enabled.');
          return;
        }
        await $.command.run({command: 'background', args: e.args});
      }
      catch { await $.ui.log('Hardcore could not dispatch the guarded handoff.'); }
    });
    return {text: 'Connecting this session to Hardcore.'};
  });
}
