// Repro client for pgjdbc: does the comment seen at Parse time go stale?
//
// Usage: java -cp postgresql.jar Repro.java <jdbc-url>
// Extra (non-pgjdbc) URL options, stripped before connecting:
//   reusePs=true  CONTROL: one PreparedStatement (created with ctx=c0) is reused
//                 for every context -> must be stale
//   clients=N     N connections used round-robin (pgbouncer scenarios)
//
// Otherwise the usual application pattern: conn.prepareStatement(sql) per call,
// where sql = "SELECT ?::text AS want /*ctx=cN*/" and ? = 'cN'. pgjdbc counts
// executions per SQL string (statement cache) and switches to a named
// server-side statement after prepareThreshold uses.
import java.sql.*;
import java.util.*;

public class Repro {
    static List<String> workload() {
        List<String> w = new ArrayList<>();
        for (int c = 0; c < 3; c++) for (int i = 0; i < 7; i++) w.add("c" + c);
        for (int i = 0; i < 9; i++) w.add("c" + (i % 3));
        return w;
    }

    public static void main(String[] args) throws Exception {
        String url = args[0];
        boolean reusePs = url.contains("reusePs=true");
        int clients = 1;
        var m = java.util.regex.Pattern.compile("clients=(\\d+)").matcher(url);
        if (m.find()) clients = Integer.parseInt(m.group(1));
        url = url.replaceAll("&(reusePs|clients)=[^&]*", "");

        List<Connection> conns = new ArrayList<>();
        for (int i = 0; i < clients; i++) conns.add(DriverManager.getConnection(url + "&ApplicationName=repro-" + i));
        DatabaseMetaData md = conns.get(0).getMetaData();
        System.out.println("pgjdbc version " + md.getDriverVersion() + " url=" + url
                + " reusePs=" + reusePs + " clients=" + clients);

        Map<Connection, PreparedStatement> shared = new HashMap<>();
        if (reusePs)
            for (Connection c : conns)
                shared.put(c, c.prepareStatement("SELECT ?::text AS want /*ctx=c0*/"));

        List<String> w = workload();
        for (int i = 0; i < w.size(); i++) {
            String want = w.get(i);
            Connection c = conns.get(i % conns.size());
            PreparedStatement ps = reusePs ? shared.get(c)
                    : c.prepareStatement("SELECT ?::text AS want /*ctx=" + want + "*/");
            ps.setString(1, want);
            try (ResultSet rs = ps.executeQuery()) {
                rs.next();
                if (!want.equals(rs.getString(1))) throw new IllegalStateException("bad result");
            }
            if (!reusePs) ps.close();
        }
        for (Connection c : conns) c.close();
        System.out.println("done: " + w.size() + " calls");
    }
}
