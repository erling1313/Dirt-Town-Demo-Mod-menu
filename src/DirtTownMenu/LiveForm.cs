using System.Diagnostics;
using System.Text.Json;

namespace DirtTownMenu;

internal sealed class LiveForm:Form
{
    record Item(int Id,string Name){public override string ToString()=>Name;}
    record Model(string Code,string Name){public override string ToString()=>Name;}
    LiveBridge? bridge;
    readonly ComboBox materials=new(){DropDownStyle=ComboBoxStyle.DropDownList},vehicles=new(){DropDownStyle=ComboBoxStyle.DropDownList},models=new(){DropDownStyle=ComboBoxStyle.DropDownList};
    readonly Label status=new(){AutoSize=true,MaximumSize=new Size(490,0)};
    readonly List<Button> buttons=[];
    bool busy;
    public LiveForm(string backupRoot)
    {
        Text="Materialien & Fahrzeuge · Echtzeit · Experimentell";ClientSize=new Size(550,760);Font=new Font("Segoe UI",10);StartPosition=FormStartPosition.CenterParent;
        var panel=new FlowLayoutPanel {Dock=DockStyle.Fill,FlowDirection=FlowDirection.TopDown,WrapContents=false,Padding=new Padding(20),AutoScroll=true};Controls.Add(panel);
        void Label(string s)=>panel.Controls.Add(new Label {Text=s,AutoSize=true,MaximumSize=new Size(490,0),Margin=new Padding(0,8,0,5)});
        Button Button(string title,Func<Task> action)
        {
            var b=new Button {Text=title,Width=490,Height=35};buttons.Add(b);panel.Controls.Add(b);
            b.Click+=async(_,_)=>{if(busy)return;busy=true;buttons.ForEach(x=>x.Enabled=false);try{await action();}catch(Exception ex){status.Text=ex.Message;}finally{busy=false;buttons.ForEach(x=>x.Enabled=true);}};return b;
        }
        Label("Lade einen Spielstand und setze das Spiel fort. Experimenteller Koop-Modus: Beide Spieler benoetigen das Menue und muessen die Synchronisation aktivieren, bevor der Host Fahrzeuge erstellt. Beim Verbinden wird ein Backup erstellt. F6 zeigt/versteckt das Fenster.");
        Button("Verbinden & Materialien / Fahrzeuge auslesen",async()=>
        {
            if(bridge is null)
            {
                var games=Process.GetProcessesByName("dirttown");if(games.Length!=1)throw new IOException("Bitte nur eine Instanz des Spiels oeffnen.");
                string backup=await Task.Run(()=>ModSession.CreateBackup(backupRoot));
                bridge=await Task.Run(()=>new LiveBridge(games[0]));
            }
            using var result=await bridge.Request(1);LoadItems(result.RootElement);status.Text="Liste aus dem aktuellen Spielstand geladen.";
        });
        Button("Koop-Synchronisation aktivieren (auf beiden PCs)",async()=>
        {
            if(bridge is null)throw new IOException("Bitte zuerst mit dem Spiel verbinden.");
            await bridge.EnableCoop();status.Text=bridge.CoopStatus;
        });
        var syncStatus=new Label {AutoSize=true,MaximumSize=new Size(490,0)};panel.Controls.Add(syncStatus);
        var syncTimer=new System.Windows.Forms.Timer{Interval=1000};
        syncTimer.Tick+=(_,_)=>syncStatus.Text=bridge?.CoopStatus??"";syncTimer.Start();
        Label("Die Synchronisation deckt neue Fahrzeuge ab. Haufen, Beladung und das Entsperren sind im Koop noch ungetestet.");
        Label("Material und Ziel-Muldenkipper");materials.Width=vehicles.Width=490;panel.Controls.Add(materials);panel.Controls.Add(vehicles);
        var amount=new NumericUpDown {Minimum=0.1m,Maximum=20,Value=5,DecimalPlaces=1,Increment=1,Width=490};Label("Menge in Muldenkipper fuellen (m³; bis zur physischen Kapazitaet)");panel.Controls.Add(amount);
        Button("Material in ausgewaehlten Muldenkipper fuellen",async()=>
        {
            if(bridge is null||materials.SelectedItem is not Item mat||vehicles.SelectedItem is not Item vehicle)throw new IOException("Verbinde dich und waehle ein Material sowie einen Muldenkipper.");
            using var result=await bridge.Request(2,vehicle.Id,mat.Id,(float)amount.Value);
            status.Text=$"Das Spiel hat {result.RootElement.GetProperty("added").GetDouble():0.00} m³ hinzugefuegt. Pruefe die Ladeflaeche.";
        });
        Button("Muldenkipper bis zum Maximum fuellen",async()=>
        {
            if(bridge is null||materials.SelectedItem is not Item mat||vehicles.SelectedItem is not Item vehicle)throw new IOException("Verbinde dich und waehle Material und Muldenkipper.");
            using var result=await bridge.Request(2,vehicle.Id,mat.Id,0);status.Text=$"{result.RootElement.GetProperty("added").GetDouble():0.00} m³ hinzugefuegt (Kapazitaetsgrenze des Spiels).";
        });
        Label("Zusaetzliches Fahrzeug spawnen");models.Width=490;
        models.Items.AddRange(new Model[]{new("dozer","Planierraupe (Dozer)"),new("hauler","Muldenkipper (Hauler)"),new("bandvagn","Kettenfahrzeug (Bandvagn)"),new("truck","LKW (Truck)"),new("excavator","Bagger (Excavator)"),new("large_loader","Grosser Radlader"),new("medium_loader","Mittlerer Radlader"),new("tractor","Traktor"),new("dirtbike","Motocross-Bike"),new("snowmobile","Schneemobil"),new("hooklift","Abrollkipper (Hooklift)"),new("feeder","Beschicker (Feeder)")});models.SelectedIndex=0;panel.Controls.Add(models);
        var near=new CheckBox {Text="Neben mir spawnen (automatische Position)",Checked=true,AutoSize=true};panel.Controls.Add(near);
        var coordinates=new FlowLayoutPanel {Width=490,Height=35,Enabled=false};var x=new NumericUpDown {Minimum=-10000,Maximum=10000,DecimalPlaces=1,Width=180};var z=new NumericUpDown {Minimum=-10000,Maximum=10000,DecimalPlaces=1,Width=180};coordinates.Controls.Add(new Label {Text="X",AutoSize=true});coordinates.Controls.Add(x);coordinates.Controls.Add(new Label {Text="Z",AutoSize=true});coordinates.Controls.Add(z);panel.Controls.Add(coordinates);
        near.CheckedChanged+=(_,_)=>coordinates.Enabled=!near.Checked;
        async Task Position(float distance)
        {
            if(!near.Checked)return;
            if(bridge is null)throw new IOException("Bitte zuerst verbinden.");
            using var snapshot=await bridge.Request(1);
            var root=snapshot.RootElement;int id=root.GetProperty("selected").GetInt32();
            var player=root.GetProperty("vehicles").EnumerateArray().FirstOrDefault(v=>v.GetProperty("id").GetInt32()==id);
            if(player.ValueKind==JsonValueKind.Undefined)throw new IOException("Spielerposition konnte nicht ermittelt werden.");
            float px=player.GetProperty("x").GetSingle(),pz=player.GetProperty("z").GetSingle();
            float bestX=px+distance,bestZ=pz,bestClearance=-1;
            for(int i=0;i<24;i++)
            {
                float angle=i*MathF.Tau/24,cx=px+distance*MathF.Cos(angle),cz=pz+distance*MathF.Sin(angle),clearance=float.MaxValue;
                foreach(var m in root.GetProperty("vehicles").EnumerateArray())
                {
                    if(m.GetProperty("name").GetString()!.StartsWith("walker",StringComparison.OrdinalIgnoreCase))continue;
                    float dx=cx-m.GetProperty("x").GetSingle(),dz=cz-m.GetProperty("z").GetSingle();clearance=Math.Min(clearance,dx*dx+dz*dz);
                }
                if(clearance>bestClearance){bestX=cx;bestZ=cz;bestClearance=clearance;}
            }
            if(bestClearance<16)throw new IOException("Zu viele Fahrzeuge in der Naehe. Begib dich auf eine freie Flaeche.");
            x.Value=Math.Clamp((decimal)bestX,x.Minimum,x.Maximum);z.Value=Math.Clamp((decimal)bestZ,z.Minimum,z.Maximum);
        }
        var radius=new NumericUpDown {Minimum=0.25m,Maximum=5,DecimalPlaces=2,Value=2,Width=225};var height=new NumericUpDown {Minimum=0.1m,Maximum=3,DecimalPlaces=1,Value=1,Width=225};
        Label("Materialhaufen: Radius / Hoehe in Metern");panel.Controls.Add(radius);panel.Controls.Add(height);
        Button("Materialhaufen neben mir erstellen",async()=>
        {
            if(bridge is null||materials.SelectedItem is not Item mat)throw new IOException("Verbinde dich und waehle ein Material.");
            await Position((float)radius.Value+5);
            using var result=await bridge.Request(5,material:mat.Id,amount:(float)radius.Value,x:(float)x.Value,z:(float)z.Value,height:(float)height.Value);status.Text="Haufen wurde vom Spiel erstellt. Pruefe das Gelaende.";
        });
        Button("Fahrzeug neben mir spawnen",async()=>
        {
            if(bridge is null)throw new IOException("Bitte zuerst verbinden.");
            await Position(18);
            using var result=await bridge.Request(3,x:(float)x.Value,z:(float)z.Value,model:((Model)models.SelectedItem!).Code);
            status.Text="Fahrzeug mit Index "+result.RootElement.GetProperty("vehicle").GetInt32()+" erstellt und entsperrt.";
        });
        Button("Alle Fahrzeuge/Bagger entsperren",async()=>
        {
            if(bridge is null)throw new IOException("Bitte zuerst verbinden.");using var result=await bridge.Request(4);
            status.Text="Entfernte Sperren: "+result.RootElement.GetProperty("unlocked").GetInt32()+".";
        });
        status.AutoSize=false;status.Dock=DockStyle.Bottom;status.Height=82;status.Padding=new Padding(15,8,15,8);status.BackColor=Color.LightGoldenrodYellow;Controls.Add(status);
        status.Text="Automatische Platzierung aktiv. Stelle dich vor dem Spawnen auf eine freie Flaeche.";
        FormClosing+=(_,e)=>{if(busy)e.Cancel=true;};FormClosed+=(_,_)=>{syncTimer.Dispose();bridge?.Dispose();};
    }
    void LoadItems(JsonElement root)
    {
        materials.Items.Clear();vehicles.Items.Clear();
        var names=new Dictionary<string,string>{{"sand","Sand"},{"gravel","Kies"},{"clay","Lehm / Ton"},{"asphalt","Asphalt"},{"ice","Eis"},{"snow","Schnee"},{"rock","Fels / Gestein"},{"roadmix","Strassenbaumix"},{"concrete","Beton"},{"duff","Waldboden / Humus"},{"topsoil","Mutterboden"},{"moraine","Moraene"},{"blue_ore","Blauerz"},{"paint_rock","Farbstein"},{"blackjack","Schwarzgestein"},{"dirt","Erde"}};
        foreach(var m in root.GetProperty("materials").EnumerateArray()){string name=m.GetProperty("name").GetString()!;materials.Items.Add(new Item(m.GetProperty("id").GetInt32(),names.GetValueOrDefault(name,name)));}
        foreach(var m in root.GetProperty("vehicles").EnumerateArray())if(m.GetProperty("name").GetString()!.Contains("hauler",StringComparison.OrdinalIgnoreCase))vehicles.Items.Add(new Item(m.GetProperty("id").GetInt32(),m.GetProperty("name").GetString()!));
        if(materials.Items.Count>0)materials.SelectedIndex=0;if(vehicles.Items.Count>0)vehicles.SelectedIndex=0;
    }
}
