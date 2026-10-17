// Formats a date, which reaches ICU's SimpleDateFormat::subFormat and the function-local
// static it initializes there, whose destructor registers through __cxa_atexit.
export default {
  async fetch(req) {
    const d = new Date(1700000000000);
    const parts = [
      new Intl.DateTimeFormat('de-DE', {
        dateStyle: 'full',
        timeStyle: 'long',
      }).format(d),
      new Intl.DateTimeFormat('ja-JP', {
        dateStyle: 'long',
        timeStyle: 'medium',
      }).format(d),
      new Intl.NumberFormat('ja-JP', {
        style: 'currency',
        currency: 'JPY',
      }).format(1234.5),
      new Intl.Collator('fr-FR', { sensitivity: 'base' }).compare('a', 'b'),
      new Intl.PluralRules('ru').select(3),
      new Intl.RelativeTimeFormat('es').format(-1, 'day'),
      'HÉLLO'.toLocaleLowerCase('tr'),
    ];
    return new Response(parts.join('|') + '\n');
  },
};
