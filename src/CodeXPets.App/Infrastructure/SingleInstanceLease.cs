using System.Threading;

namespace CodeXPets.App.Infrastructure;

internal sealed class SingleInstanceLease : IDisposable
{
    private readonly Mutex? _mutex;

    private SingleInstanceLease(Mutex? mutex, bool acquired)
    {
        _mutex = mutex;
        Acquired = acquired;
    }

    public bool Acquired { get; }

    public static SingleInstanceLease TryAcquire()
    {
        try
        {
            var mutex = new Mutex(initiallyOwned: true,
                name: "CodeXPets_4B6B725D_C578_47C7_A88D_AA6E548D1ED8",
                createdNew: out var createdNew);
            return new SingleInstanceLease(mutex, createdNew);
        }
        catch
        {
            return new SingleInstanceLease(null, acquired: true);
        }
    }

    public void Dispose()
    {
        if (_mutex is null) return;
        if (Acquired)
        {
            try { _mutex.ReleaseMutex(); } catch (ApplicationException) { }
        }
        _mutex.Dispose();
    }
}
